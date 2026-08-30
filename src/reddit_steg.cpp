#include "reddit_steg.h"

#include "encryption_internal.h"
#include "file_utils.h"
#include "signal_utils.h"

#include <algorithm>
#include <array>
#include <csetjmp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <jpeglib.h>
}

namespace {

constexpr auto RQSTEG_MAGIC = std::to_array<Byte>({
    'R', 'Q', 'S', 'T', 'E', 'G', '1', 0
});
constexpr auto REDDIT_ENVELOPE_MAGIC = std::to_array<Byte>({
    'J', 'D', 'V', 'R', 'I', 'F', 'R', '1'
});

constexpr Byte
    RQSTEG_VERSION          = 1,
    HEADER_COPIES           = 17,
    PAYLOAD_COPIES          = 3,
    ENVELOPE_FLAG_COMPRESSED = 1;

constexpr int
    QIM_STEP           = 8,
    CARRIER_QUALITY    = 75,
    MIN_IMAGE_DIMENSION = 400;

// Carrier keying.
//
// Every coefficient position and every whitening bit derives from a
// `carrier_key` supplied by the caller, which is a cheap hash of the recovery
// PIN (see deriveCarrierKeyFromPin). It used to be a hard-coded constant, with
// the consequence that anyone holding the source could recompute the header
// coefficient positions, undo the whitening and test any image for the rqsteg
// magic and its CRC -- the carrier was locatable without the PIN.
//
// Keying it from the PIN removes that: without the PIN the positions are
// unknown, so "does this image carry a payload?" stops being answerable by
// anyone but the holder.
//
// This protects *position*, not contents; the payload underneath is
// secretstream-encrypted under the Argon2id key either way.
constexpr std::uint64_t
    HEADER_DOMAIN    = 0x4845414445527631ULL,
    PAYLOAD_DOMAIN   = 0x5041594c4f414431ULL;

constexpr std::size_t
    RQSTEG_HEADER_SIZE      = 24,
    RQSTEG_HEADER_BITS      = RQSTEG_HEADER_SIZE * 8,
    ENVELOPE_PREFIX_BYTES   = 16,
    REDDIT_ENVELOPE_HEADER_BYTES =
        ENVELOPE_PREFIX_BYTES + KDF_METADATA_REGION_BYTES;

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

constexpr std::array<unsigned int, 64> STD_LUMA_Q50{
    16,11,10,16,24,40,51,61,
    12,12,14,19,26,58,60,55,
    14,13,16,24,40,57,69,56,
    14,17,22,29,51,87,80,62,
    18,22,37,56,68,109,103,77,
    24,35,55,64,81,104,113,92,
    49,64,78,87,103,121,120,101,
    72,92,95,98,112,100,103,99
};

constexpr std::array<unsigned int, 64> STD_CHROMA_Q50{
    17,18,24,47,99,99,99,99,
    18,21,26,66,99,99,99,99,
    24,26,56,99,99,99,99,99,
    47,66,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99
};

struct JpegErrorManager {
    jpeg_error_mgr pub{};
    std::jmp_buf jump{};
    std::array<char, JMSG_LENGTH_MAX> message{};
};

extern "C" void jpegErrorExit(j_common_ptr cinfo) {
    auto* error = reinterpret_cast<JpegErrorManager*>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, error->message.data());
    std::longjmp(error->jump, 1);
}

struct CodecState {
    jpeg_decompress_struct source{};
    jpeg_compress_struct destination{};
    unsigned char* output_buffer{nullptr};
    unsigned long output_size{0};
    bool source_created{false};
    bool destination_created{false};
};

struct ImageInfo {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t y_blocks_wide{0};
    std::uint32_t y_blocks_high{0};
    int components{0};
    bool progressive{false};
    bool is_420{false};
    bool is_q75{false};

    [[nodiscard]] std::uint64_t yBlockCount() const noexcept {
        return static_cast<std::uint64_t>(y_blocks_wide) * y_blocks_high;
    }
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
// bounded by carrierCapacity() * 8.
struct Carrier {
    std::uint32_t bit_index{0};
    std::uint16_t block_row{0};
    std::uint16_t block_column{0};
    Byte coefficient_index{0};
    bool whitening_bit{false};
};

// 24 -> 12 bytes. At REDDIT_COVER_IMAGE_LIMITS.max_pixels this is the
// program's largest allocation, so halving it halves the peak RSS of a Reddit
// conceal/recover.
static_assert(sizeof(Carrier) == 12, "Carrier is expected to stay a 12-byte record.");

struct CarrierHeader {
    std::uint32_t payload_size{0};
    std::uint32_t payload_crc{0};
};

struct DecodedSymbol {
    bool bit{false};
    int confidence{0};
};

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
// CodecState and JpegErrorManager are already heap-allocated for the same
// reason; this extends the treatment to the rest. (ImageInfo and the plain
// scalars stay on the stack: they are trivially destructible and are never
// read on the longjmp path, so no indeterminate value is ever observed.)
struct CodecLocals {
    std::vector<Carrier> header_carriers;
    std::vector<Carrier> payload_carriers;
    std::vector<JSAMPLE> row;
    vBytes header_bytes;
    vBytes payload_bytes;
    vBytes output;
    std::optional<CarrierHeader> header;
};

constexpr const char* REDDIT_CORRUPT_ERROR =
    "File Extraction Error: Reddit embedded data is corrupt!";

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void cleanupCodec(CodecState& state) noexcept {
    if (state.destination_created) {
        jpeg_destroy_compress(&state.destination);
        state.destination_created = false;
    }
    if (state.source_created) {
        jpeg_destroy_decompress(&state.source);
        state.source_created = false;
    }
    std::free(state.output_buffer);
    state.output_buffer = nullptr;
    state.output_size = 0;
}

void requireMemorySourceSize(std::size_t size) {
    if (size == 0 || size > static_cast<std::size_t>(std::numeric_limits<unsigned long>::max())) {
        fail("Image Error: JPEG input is empty or too large for the decoder.");
    }
}

[[nodiscard]] vBytes copyCodecOutput(const CodecState& state) {
    if (state.output_buffer == nullptr || state.output_size == 0) {
        fail("Image Error: JPEG encoder produced no output.");
    }
    const std::size_t size = static_cast<std::size_t>(state.output_size);
    vBytes output(size);
    std::memcpy(output.data(), state.output_buffer, size);
    return output;
}

[[nodiscard]] std::uint32_t crc32(std::span<const Byte> data) {
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

    if (!spanHasRange(input, offset, 4)) fail(REDDIT_CORRUPT_ERROR);
    std::uint32_t value = 0;
    for (unsigned int byte = 0; byte < 4; ++byte) {
        value |= static_cast<std::uint32_t>(input[offset + byte])
            << (byte * 8U);
    }
    return value;
}

[[nodiscard]] vBytes makeCarrierHeader(
    std::uint32_t payload_size,
    std::uint32_t payload_crc) {

    vBytes header;
    header.reserve(RQSTEG_HEADER_SIZE);
    header.insert(header.end(), RQSTEG_MAGIC.begin(), RQSTEG_MAGIC.end());
    header.push_back(RQSTEG_VERSION);
    header.push_back(static_cast<Byte>(QIM_STEP));
    header.push_back(PAYLOAD_COPIES);
    header.push_back(0); // flags
    appendU32(header, payload_size);
    appendU32(header, payload_crc);
    appendU32(header, crc32(header));
    if (header.size() != RQSTEG_HEADER_SIZE) {
        fail("Internal Error: Reddit carrier header has an invalid size.");
    }
    return header;
}

[[nodiscard]] std::optional<CarrierHeader> parseCarrierHeader(
    std::span<const Byte> header) {

    if (header.size() != RQSTEG_HEADER_SIZE) {
        fail(REDDIT_CORRUPT_ERROR);
    }
    if (!std::equal(RQSTEG_MAGIC.begin(), RQSTEG_MAGIC.end(), header.begin())) {
        return std::nullopt;
    }

    const std::uint32_t expected_header_crc = readU32(header, 20);
    if (crc32(header.first(20)) != expected_header_crc) {
        fail("File Extraction Error: Reddit carrier header failed its integrity check.");
    }
    if (header[8] != RQSTEG_VERSION ||
        header[9] != static_cast<Byte>(QIM_STEP) ||
        header[10] != PAYLOAD_COPIES ||
        header[11] != 0) {
        fail("File Extraction Error: Unsupported Reddit carrier format.");
    }

    return CarrierHeader{
        .payload_size = readU32(header, 12),
        .payload_crc = readU32(header, 16),
    };
}

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

[[nodiscard]] std::uint64_t mix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

// Upper bound on an affine permutation's domain. `multiplier * index` must not
// wrap: modular arithmetic on a wrapped product is no longer a bijection, and
// two payload bits landing on one coefficient would corrupt the extraction
// silently rather than failing. Both factors are < count, so count <= 2^32
// makes the product exact. The Reddit pixel ceiling keeps real domains well
// below this, but the invariant is what the permutation depends on, so it is
// checked rather than inferred.
constexpr std::uint64_t MAX_PERMUTATION_COUNT = 1ULL << 32;

struct AffinePermutation {
    std::uint64_t count{0};
    std::uint64_t multiplier{0};
    std::uint64_t offset{0};

    // Exact for every count <= MAX_PERMUTATION_COUNT (enforced in makePermutation).
    [[nodiscard]] std::uint64_t operator()(std::uint64_t index) const noexcept {
        return ((multiplier * index) + offset) % count;
    }
};

[[nodiscard]] AffinePermutation makePermutation(
    std::uint64_t count,
    std::uint64_t seed,
    std::uint64_t domain) {

    if (count == 0) {
        fail("Image Error: Carrier image has no usable coefficient positions.");
    }
    if (count > MAX_PERMUTATION_COUNT) {
        fail("Internal Error: Reddit carrier permutation domain is too large.");
    }
    std::uint64_t multiplier = mix64(seed ^ domain ^ count) % count;
    if (multiplier == 0) multiplier = 1;
    while (std::gcd(multiplier, count) != 1U) {
        multiplier = (multiplier + 1U) % count;
        if (multiplier == 0) multiplier = 1;
    }
    return AffinePermutation{
        .count = count,
        .multiplier = multiplier,
        .offset = mix64(seed + domain) % count,
    };
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
// no O(n log n) pass over what is the program's largest allocation. Each bucket
// is then ordered by column so the loops walk a block row forwards instead of
// hopping around it.
template<std::size_t FrequencyCount>
[[nodiscard]] std::vector<Carrier> makeCarriers(
    const ImageInfo& info,
    const std::array<Byte, FrequencyCount>& frequencies,
    std::uint64_t bit_count,
    Byte copies,
    std::uint64_t seed,
    std::uint64_t domain) {

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

    // makePermutation caps candidate_count at 2^32, so every candidate fits in
    // 32 bits and the row split below stays 32-bit arithmetic. It also rejects
    // an empty domain, which is the only way y_blocks_wide could be zero -- so
    // row_stride is a safe divisor from here on.
    const AffinePermutation permutation = makePermutation(candidate_count, seed, domain);
    const std::uint32_t row_stride =
        static_cast<std::uint32_t>(FrequencyCount) * info.y_blocks_wide;

    // Pass one: bucket sizes, stored shifted by one so the prefix sum turns
    // them directly into per-row write cursors.
    std::vector<std::size_t> row_cursor(static_cast<std::size_t>(info.y_blocks_high) + 1, 0);
    for (std::uint64_t slot = 0; slot < slots; ++slot) {
        const std::uint32_t candidate = static_cast<std::uint32_t>(permutation(slot));
        ++row_cursor[static_cast<std::size_t>(candidate / row_stride) + 1];
    }
    for (std::size_t row = 1; row < row_cursor.size(); ++row) {
        row_cursor[row] += row_cursor[row - 1];
    }

    // Pass two: scatter into the bucket each candidate was counted into.
    std::vector<Carrier> carriers(static_cast<std::size_t>(slots));
    for (std::uint64_t bit = 0; bit < bit_count; ++bit) {
        for (std::uint64_t copy = 0; copy < copy_count; ++copy) {
            const std::uint64_t slot = (bit * copy_count) + copy;
            const std::uint32_t candidate = static_cast<std::uint32_t>(permutation(slot));
            const std::uint32_t row = candidate / row_stride;
            const std::uint32_t within = candidate - (row * row_stride);
            const std::uint32_t column = within / static_cast<std::uint32_t>(FrequencyCount);
            const std::size_t frequency =
                static_cast<std::size_t>(within % FrequencyCount);
            carriers[row_cursor[row]++] = Carrier{
                .bit_index = static_cast<std::uint32_t>(bit),
                .block_row = static_cast<std::uint16_t>(row),
                .block_column = static_cast<std::uint16_t>(column),
                .coefficient_index = frequencies[frequency],
                .whitening_bit = (mix64(seed ^ domain ^ candidate) & 1U) != 0U,
            };
        }
    }

    // Order each row's bucket by column. Buckets are small (slots / blocks_high
    // on average), so this is far cheaper than sorting the whole array and it
    // keeps the row-array access pattern sequential.
    std::size_t bucket_start = 0;
    for (std::uint32_t row = 0; row < info.y_blocks_high; ++row) {
        const std::size_t bucket_end = row_cursor[row];
        if (bucket_end - bucket_start > 1) {
            std::sort(
                carriers.begin() + static_cast<std::ptrdiff_t>(bucket_start),
                carriers.begin() + static_cast<std::ptrdiff_t>(bucket_end),
                [](const Carrier& lhs, const Carrier& rhs) {
                    if (lhs.block_column != rhs.block_column) {
                        return lhs.block_column < rhs.block_column;
                    }
                    return lhs.coefficient_index < rhs.coefficient_index;
                });
        }
        bucket_start = bucket_end;
    }

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

[[nodiscard]] JCOEF qimEncode(JCOEF coefficient, bool bit) {
    constexpr int COEF_MIN = static_cast<int>(std::numeric_limits<JCOEF>::min());
    constexpr int COEF_MAX = static_cast<int>(std::numeric_limits<JCOEF>::max());

    const int value = coefficient;
    const int target = bit ? (3 * QIM_STEP) / 4 : QIM_STEP / 4;
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
    if (chosen > COEF_MAX) {
        chosen = lower;
    } else if (chosen < COEF_MIN) {
        chosen = upper;
    }
    if (chosen > COEF_MAX || chosen < COEF_MIN) {
        fail("Internal Error: Reddit carrier coefficient left its quantization lattice.");
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

[[nodiscard]] DecodedSymbol qimDecode(JCOEF coefficient) noexcept {
    const int residue = positiveModulo(coefficient, QIM_STEP);
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

[[nodiscard]] std::array<unsigned int, 64> scaledQuantTable(
    const std::array<unsigned int, 64>& base,
    int quality) {

    const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    std::array<unsigned int, 64> result{};
    for (std::size_t i = 0; i < base.size(); ++i) {
        const int value = std::clamp(
            (static_cast<int>(base[i]) * scale + 50) / 100,
            1,
            255);
        result[i] = static_cast<unsigned int>(value);
    }
    return result;
}

void installExactQuantTable(
    jpeg_compress_struct& destination,
    int table_index,
    const std::array<unsigned int, 64>& values) {

    if (table_index < 0 || table_index >= NUM_QUANT_TBLS) {
        fail("Internal Error: Reddit quantization-table index is invalid.");
    }

    JQUANT_TBL*& table =
        destination.quant_tbl_ptrs[table_index];
    if (table == nullptr) {
        table = jpeg_alloc_quant_table(
            reinterpret_cast<j_common_ptr>(&destination));
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
        table->quantval[i] = static_cast<UINT16>(values[i]);
    }
    table->sent_table = FALSE;
}

void installStandardQ75QuantTables(
    jpeg_compress_struct& destination) {

    installExactQuantTable(
        destination,
        0,
        scaledQuantTable(STD_LUMA_Q50, CARRIER_QUALITY));
    installExactQuantTable(
        destination,
        1,
        scaledQuantTable(STD_CHROMA_Q50, CARRIER_QUALITY));

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
    const bool has_three_components = source.num_components == 3;
    const bool is_420 = has_three_components &&
        source.comp_info[0].h_samp_factor == 2 &&
        source.comp_info[0].v_samp_factor == 2 &&
        source.comp_info[1].h_samp_factor == 1 &&
        source.comp_info[1].v_samp_factor == 1 &&
        source.comp_info[2].h_samp_factor == 1 &&
        source.comp_info[2].v_samp_factor == 1;
    const auto expected_luma = scaledQuantTable(STD_LUMA_Q50, CARRIER_QUALITY);
    const auto expected_chroma = scaledQuantTable(STD_CHROMA_Q50, CARRIER_QUALITY);
    const bool is_q75 = has_three_components &&
        quantTableMatches(source.quant_tbl_ptrs[0], expected_luma) &&
        quantTableMatches(source.quant_tbl_ptrs[1], expected_chroma);

    return ImageInfo{
        .width = static_cast<std::uint32_t>(source.image_width),
        .height = static_cast<std::uint32_t>(source.image_height),
        .y_blocks_wide = has_three_components
            ? static_cast<std::uint32_t>(source.comp_info[0].width_in_blocks) : 0,
        .y_blocks_high = has_three_components
            ? static_cast<std::uint32_t>(source.comp_info[0].height_in_blocks) : 0,
        .components = source.num_components,
        .progressive = source.progressive_mode != 0,
        .is_420 = is_420,
        .is_q75 = is_q75,
    };
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
        info.is_420 &&
        info.is_q75 &&
        info.width >= MIN_IMAGE_DIMENSION &&
        info.height >= MIN_IMAGE_DIMENSION &&
        info.width <= REDDIT_COVER_IMAGE_LIMITS.max_dimension &&
        info.height <= REDDIT_COVER_IMAGE_LIMITS.max_dimension &&
        pixel_count <= REDDIT_COVER_IMAGE_LIMITS.max_pixels &&
        info.yBlockCount() >=
            static_cast<std::uint64_t>(RQSTEG_HEADER_BITS) * HEADER_COPIES;
}

void validateCarrier(const ImageInfo& info) {
    if (info.components != 3) {
        fail("Internal Error: Reddit carrier is not a three-component colour JPEG.");
    }
    if (info.progressive) {
        fail("Internal Error: Reddit carrier is not a baseline JPEG.");
    }
    if (!info.is_420) {
        fail("Internal Error: Reddit carrier does not use YCbCr 4:2:0 subsampling.");
    }
    if (!info.is_q75) {
        fail("Internal Error: Reddit carrier does not use the standard quality-75 quantization tables.");
    }
    if (info.yBlockCount() <
        static_cast<std::uint64_t>(RQSTEG_HEADER_BITS) * HEADER_COPIES) {
        fail("Image Error: Cover image is too small for the robust Reddit carrier header.");
    }
}

[[nodiscard]] ImageInfo inspectJpeg(std::span<const Byte> input) {
    requireMemorySourceSize(input.size());
    auto state = std::make_unique<CodecState>();
    auto error = std::make_unique<JpegErrorManager>();
    ImageInfo info{};

    jpeg_std_error(&error->pub);
    error->pub.error_exit = jpegErrorExit;
    if (setjmp(error->jump) != 0) {
        const std::string message = error->message.data();
        cleanupCodec(*state);
        fail("JPEG error: " + message);
    }

    state->source.err = &error->pub;
    jpeg_create_decompress(&state->source);
    state->source_created = true;
    jpeg_mem_src(
        &state->source,
        input.data(),
        static_cast<unsigned long>(input.size()));
    jpeg_read_header(&state->source, TRUE);
    info = imageInfoFromHeader(state->source);
    cleanupCodec(*state);
    return info;
}

[[nodiscard]] vBytes transcodeCarrier(std::span<const Byte> input) {
    requireMemorySourceSize(input.size());
    auto state = std::make_unique<CodecState>();
    auto error = std::make_unique<JpegErrorManager>();
    auto locals = std::make_unique<CodecLocals>();

    jpeg_std_error(&error->pub);
    error->pub.error_exit = jpegErrorExit;
    if (setjmp(error->jump) != 0) {
        const std::string message = error->message.data();
        cleanupCodec(*state);
        fail("JPEG error while preparing Reddit cover: " + message);
    }

    try {
        state->source.err = &error->pub;
        jpeg_create_decompress(&state->source);
        state->source_created = true;
        jpeg_mem_src(
            &state->source,
            input.data(),
            static_cast<unsigned long>(input.size()));
        jpeg_read_header(&state->source, TRUE);
        validateCoverInput(state->source);
        state->source.out_color_space = JCS_RGB;
        jpeg_start_decompress(&state->source);

        state->destination.err = &error->pub;
        jpeg_create_compress(&state->destination);
        state->destination_created = true;
        jpeg_mem_dest(
            &state->destination,
            &state->output_buffer,
            &state->output_size);
        state->destination.image_width = state->source.output_width;
        state->destination.image_height = state->source.output_height;
        state->destination.input_components = 3;
        state->destination.in_color_space = JCS_RGB;
        jpeg_set_defaults(&state->destination);
        // "Quality 75" is not a portable table specification: compatible
        // libjpeg implementations may map it to different defaults. Install
        // the exact IJG Q75 tables required by the Reddit carrier instead.
        installStandardQ75QuantTables(state->destination);
        state->destination.comp_info[0].h_samp_factor = 2;
        state->destination.comp_info[0].v_samp_factor = 2;
        state->destination.comp_info[1].h_samp_factor = 1;
        state->destination.comp_info[1].v_samp_factor = 1;
        state->destination.comp_info[2].h_samp_factor = 1;
        state->destination.comp_info[2].v_samp_factor = 1;
        state->destination.optimize_coding = FALSE;
        state->destination.write_JFIF_header = FALSE;
        state->destination.write_Adobe_marker = FALSE;
        jpeg_start_compress(&state->destination, TRUE);

        const std::uint64_t row_size =
            static_cast<std::uint64_t>(state->source.output_width) * 3U;
        if (row_size > std::numeric_limits<std::size_t>::max()) {
            fail("Image Error: Decoded JPEG row is too large.");
        }
        locals->row.resize(static_cast<std::size_t>(row_size));
        while (state->source.output_scanline < state->source.output_height) {
            throwIfSignalCancellationRequested();
            JSAMPROW scanline = locals->row.data();
            jpeg_read_scanlines(&state->source, &scanline, 1);
            jpeg_write_scanlines(&state->destination, &scanline, 1);
        }

        jpeg_finish_compress(&state->destination);
        jpeg_finish_decompress(&state->source);
        locals->output = copyCodecOutput(*state);
        cleanupCodec(*state);
        return std::move(locals->output);
    } catch (...) {
        cleanupCodec(*state);
        throw;
    }
}

// makeCarriers already emits carriers grouped by block_row and ordered by
// block_column within a row, so both walks below just stream the array.
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
            coefficient = qimEncode(coefficient, encoded_bit);
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
    std::uint64_t bit_count) {

    if (bit_count > static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max())) {
        fail(REDDIT_CORRUPT_ERROR);
    }
    std::vector<int> scores(static_cast<std::size_t>(bit_count), 0);
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
                block_row[0][column][carrier.coefficient_index]);
            symbol.bit = symbol.bit ^ carrier.whitening_bit;
            const std::size_t bit =
                static_cast<std::size_t>(carrier.bit_index);
            scores[bit] += symbol.bit
                ? symbol.confidence
                : -symbol.confidence;
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
    std::uint64_t carrier_key) {

    requireMemorySourceSize(input.size());
    auto state = std::make_unique<CodecState>();
    auto error = std::make_unique<JpegErrorManager>();
    auto locals = std::make_unique<CodecLocals>();

    jpeg_std_error(&error->pub);
    error->pub.error_exit = jpegErrorExit;
    if (setjmp(error->jump) != 0) {
        const std::string message = error->message.data();
        cleanupCodec(*state);
        fail("JPEG error while embedding Reddit payload: " + message);
    }

    try {
        state->source.err = &error->pub;
        jpeg_create_decompress(&state->source);
        state->source_created = true;
        jpeg_mem_src(
            &state->source,
            input.data(),
            static_cast<unsigned long>(input.size()));
        jpeg_read_header(&state->source, TRUE);
        const ImageInfo info = imageInfoFromHeader(state->source);
        validateCarrier(info);
        jvirt_barray_ptr* arrays = jpeg_read_coefficients(&state->source);

        locals->header_carriers = makeCarriers(
            info,
            HEADER_FREQUENCIES,
            RQSTEG_HEADER_BITS,
            HEADER_COPIES,
            carrier_key,
            HEADER_DOMAIN);
        applyCarriers(state->source, arrays, locals->header_carriers, header);
        // Released before the payload set is built. The two sets never share a
        // coefficient, and at the capacity ceiling the payload array is the
        // largest allocation the program makes -- no reason to hold both.
        locals->header_carriers = std::vector<Carrier>{};

        const std::uint64_t payload_bits =
            static_cast<std::uint64_t>(payload.size()) * 8U;
        locals->payload_carriers = makeCarriers(
            info,
            PAYLOAD_FREQUENCIES,
            payload_bits,
            PAYLOAD_COPIES,
            carrier_key,
            PAYLOAD_DOMAIN);
        applyCarriers(state->source, arrays, locals->payload_carriers, payload);
        locals->payload_carriers = std::vector<Carrier>{};

        state->destination.err = &error->pub;
        jpeg_create_compress(&state->destination);
        state->destination_created = true;
        jpeg_mem_dest(
            &state->destination,
            &state->output_buffer,
            &state->output_size);
        jpeg_copy_critical_parameters(&state->source, &state->destination);
        state->destination.write_JFIF_header = FALSE;
        state->destination.write_Adobe_marker = FALSE;
        state->destination.optimize_coding = FALSE;
        jpeg_write_coefficients(&state->destination, arrays);
        jpeg_finish_compress(&state->destination);
        jpeg_finish_decompress(&state->source);
        locals->output = copyCodecOutput(*state);
        cleanupCodec(*state);
        return std::move(locals->output);
    } catch (...) {
        cleanupCodec(*state);
        throw;
    }
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

[[nodiscard]] std::optional<vBytes> extractCarrierPayload(
    std::span<const Byte> input,
    std::uint64_t carrier_key) {

    if (input.size() < 2 || input[0] != 0xFF || input[1] != 0xD8) {
        return std::nullopt;
    }
    requireMemorySourceSize(input.size());

    auto state = std::make_unique<CodecState>();
    auto error = std::make_unique<JpegErrorManager>();
    auto locals = std::make_unique<CodecLocals>();

    jpeg_std_error(&error->pub);
    error->pub.error_exit = jpegErrorExit;
    if (setjmp(error->jump) != 0) {
        const std::string message = error->message.data();
        cleanupCodec(*state);
        fail("JPEG error while extracting Reddit payload: " + message);
    }

    try {
        state->source.err = &error->pub;
        jpeg_create_decompress(&state->source);
        state->source_created = true;
        jpeg_mem_src(
            &state->source,
            input.data(),
            static_cast<unsigned long>(input.size()));
        jpeg_read_header(&state->source, TRUE);
        const ImageInfo info = imageInfoFromHeader(state->source);
        if (!isCompatibleCarrier(info)) {
            cleanupCodec(*state);
            return std::nullopt;
        }

        jvirt_barray_ptr* arrays = jpeg_read_coefficients(&state->source);
        locals->header_carriers = makeCarriers(
            info,
            HEADER_FREQUENCIES,
            RQSTEG_HEADER_BITS,
            HEADER_COPIES,
            carrier_key,
            HEADER_DOMAIN);
        locals->header_bytes = readCarriers(
            state->source,
            arrays,
            locals->header_carriers,
            RQSTEG_HEADER_BITS);
        locals->header_carriers = std::vector<Carrier>{};
        locals->header = parseCarrierHeader(locals->header_bytes);
        if (!locals->header) {
            jpeg_finish_decompress(&state->source);
            cleanupCodec(*state);
            return std::nullopt;
        }

        if (locals->header->payload_size > carrierCapacity(info.yBlockCount())) {
            fail("File Extraction Error: Reddit carrier declares a payload larger than its coefficient capacity.");
        }
        // Everything from here on is sized by an attacker-controlled field that
        // only a CRC has vouched for so far -- see extractRedditEnvelope's
        // caller in recover.cpp for why that ordering is unavoidable for this
        // format, and carrierCapacity() for the ceiling that bounds it.
        const std::uint64_t payload_bits =
            static_cast<std::uint64_t>(locals->header->payload_size) * 8U;
        locals->payload_carriers = makeCarriers(
            info,
            PAYLOAD_FREQUENCIES,
            payload_bits,
            PAYLOAD_COPIES,
            carrier_key,
            PAYLOAD_DOMAIN);
        locals->payload_bytes = readCarriers(
            state->source,
            arrays,
            locals->payload_carriers,
            payload_bits);
        locals->payload_carriers = std::vector<Carrier>{};
        if (crc32(locals->payload_bytes) != locals->header->payload_crc) {
            fail("File Extraction Error: Reddit carrier payload failed its integrity check.");
        }

        jpeg_finish_decompress(&state->source);
        cleanupCodec(*state);
        return std::move(locals->payload_bytes);
    } catch (...) {
        cleanupCodec(*state);
        throw;
    }
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
        .luminance_blocks = info.yBlockCount(),
        .payload_capacity = carrierCapacity(info.yBlockCount()),
    };
}

std::size_t redditEnvelopeSize(std::size_t encrypted_size) {
    return checkedAdd(
        REDDIT_ENVELOPE_HEADER_BYTES,
        encrypted_size,
        "File Size Error: Reddit encrypted-envelope size overflow.");
}

vBytes makeRedditEnvelope(
    std::span<const Byte> kdf_metadata,
    std::span<const Byte> encrypted_data,
    bool is_compressed) {

    if (kdf_metadata.size() != KDF_METADATA_REGION_BYTES) {
        fail("Internal Error: Reddit KDF metadata has an invalid size.");
    }
    if (encrypted_data.size() > std::numeric_limits<std::uint32_t>::max()) {
        fail("File Size Error: Reddit encrypted payload exceeds its format limit.");
    }

    vBytes envelope;
    envelope.reserve(redditEnvelopeSize(encrypted_data.size()));
    envelope.insert(
        envelope.end(),
        REDDIT_ENVELOPE_MAGIC.begin(),
        REDDIT_ENVELOPE_MAGIC.end());
    envelope.push_back(is_compressed ? ENVELOPE_FLAG_COMPRESSED : 0);
    envelope.insert(envelope.end(), 3, 0); // reserved
    appendU32(envelope, static_cast<std::uint32_t>(encrypted_data.size()));
    envelope.insert(envelope.end(), kdf_metadata.begin(), kdf_metadata.end());
    envelope.insert(envelope.end(), encrypted_data.begin(), encrypted_data.end());
    if (envelope.size() != redditEnvelopeSize(encrypted_data.size())) {
        fail("Internal Error: Reddit encrypted envelope has an invalid size.");
    }
    return envelope;
}

vBytes embedRedditPayload(
    const RedditPreparedCover& cover,
    std::uint64_t carrier_key,
    std::span<const Byte> payload) {

    if (payload.size() > cover.payload_capacity) {
        fail("Data File Size Error: Encrypted Reddit payload exceeds the cover image's theoretical C3 capacity.");
    }
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        fail("Data File Size Error: Encrypted Reddit payload exceeds its format limit.");
    }

    const vBytes header = makeCarrierHeader(
        static_cast<std::uint32_t>(payload.size()),
        crc32(payload));
    return embedCarrier(cover.jpeg, header, payload, carrier_key);
}

std::optional<RedditEncryptedEnvelope> extractRedditEnvelope(
    std::span<const Byte> input_jpeg,
    std::uint64_t carrier_key) {

    std::optional<vBytes> carrier_payload =
        extractCarrierPayload(input_jpeg, carrier_key);
    if (!carrier_payload) return std::nullopt;

    const std::span<const Byte> payload(*carrier_payload);
    if (payload.size() < REDDIT_ENVELOPE_MAGIC.size() ||
        !std::equal(
            REDDIT_ENVELOPE_MAGIC.begin(),
            REDDIT_ENVELOPE_MAGIC.end(),
            payload.begin())) {
        return std::nullopt;
    }
    if (payload.size() < REDDIT_ENVELOPE_HEADER_BYTES) {
        fail(REDDIT_CORRUPT_ERROR);
    }

    const Byte flags = payload[8];
    if ((flags & static_cast<Byte>(~ENVELOPE_FLAG_COMPRESSED)) != 0 ||
        payload[9] != 0 ||
        payload[10] != 0 ||
        payload[11] != 0) {
        fail("File Extraction Error: Reddit envelope contains unsupported flags.");
    }

    const std::size_t encrypted_size = readU32(payload, 12);
    const std::size_t expected_size = checkedAdd(
        REDDIT_ENVELOPE_HEADER_BYTES,
        encrypted_size,
        REDDIT_CORRUPT_ERROR);
    if (payload.size() != expected_size) {
        fail(REDDIT_CORRUPT_ERROR);
    }

    RedditEncryptedEnvelope envelope;
    envelope.kdf_metadata.assign(
        payload.begin() + static_cast<std::ptrdiff_t>(ENVELOPE_PREFIX_BYTES),
        payload.begin() + static_cast<std::ptrdiff_t>(
            REDDIT_ENVELOPE_HEADER_BYTES));
    envelope.encrypted_data.assign(
        payload.begin() + static_cast<std::ptrdiff_t>(
            REDDIT_ENVELOPE_HEADER_BYTES),
        payload.end());
    envelope.is_compressed =
        (flags & ENVELOPE_FLAG_COMPRESSED) != 0;
    return envelope;
}
