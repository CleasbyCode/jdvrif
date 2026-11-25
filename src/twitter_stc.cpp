#include "twitter_stc.h"
#include "carrier_common.h"
#include "carrier_parallel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace twitter_steg_internal {
namespace {

constexpr std::size_t kStateCount = 1U << kStcConstraintHeight;
constexpr std::size_t kMaxSegmentCover = 65'536;

// Optimized h=7 STC submatrices distributed with the reference J-UNIWARD
// implementation by V. Holub, J. Fridrich, and T. Denemark. The fixed 2/5
// code rate needs only widths two and three.
constexpr std::array<std::uint32_t, 2> kColumnsWidth2{109U, 71U};
constexpr std::array<std::uint32_t, 3> kColumnsWidth3{109U, 79U, 83U};

struct Segment {
    std::size_t cover_offset{};
    std::size_t cover_count{};
    std::size_t message_offset{};
    std::size_t message_count{};
};

struct PathBits {
    std::array<std::uint64_t, 2> words{};
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("STC error: " + message);
}

// Calls on_width(width) with the cover columns of each message bit, in
// order, without materialising the schedule.
template<typename OnWidth>
void forEachWidth(std::size_t cover_count,
                  std::size_t message_count,
                  OnWidth&& on_width) {
    if (message_count == 0 || cover_count < message_count) {
        fail("invalid cover/message lengths.");
    }
    // Keeps (message + 1) * cover_count below 2^64 in the loop below. The
    // carriers' covers are far smaller; this bounds direct callers.
    if (cover_count > std::numeric_limits<std::uint32_t>::max()) {
        fail("cover is too long for the column schedule.");
    }
    std::size_t consumed = 0;
    for (std::size_t message = 0; message < message_count; ++message) {
        // Round each cumulative boundary to the nearest integer. This is the
        // integer equivalent of the balanced "worm" schedule in the original
        // STC implementation.
        const std::uint64_t numerator =
            static_cast<std::uint64_t>(message + 1U) * cover_count;
        const std::size_t boundary = static_cast<std::size_t>(
            (numerator + message_count / 2U) / message_count);
        const std::size_t width = boundary - consumed;
        consumed = boundary;
        if (width < 2 || width > 3) {
            fail("this profile requires STC column widths of two or three.");
        }
        on_width(width);
    }
    if (consumed != cover_count) {
        fail("column schedule did not consume the cover.");
    }
}

// The schedule of one segment, which embedRaw also walks backwards.
[[nodiscard]] std::vector<std::size_t> makeWidths(std::size_t cover_count,
                                                   std::size_t message_count) {
    std::vector<std::size_t> widths;
    widths.reserve(message_count);
    forEachWidth(cover_count, message_count,
                 [&](std::size_t width) { widths.push_back(width); });
    return widths;
}

[[nodiscard]] std::uint32_t columnFor(std::size_t width,
                                      std::size_t column) {
    if (width == kColumnsWidth2.size()) {
        return kColumnsWidth2.at(column);
    }
    if (width == kColumnsWidth3.size()) {
        return kColumnsWidth3.at(column);
    }
    fail("unsupported submatrix width.");
}

// One trellis step over all kStateCount states, kPriceLanes at a time. For
// state s,
//     zero = prices[s] + zero_add,  one = prices[s ^ column] + one_add,
//     next[s] = one <= zero ? one : zero,  decision bit s = (one <= zero),
// exactly as a scalar loop computes them, so the trellis is unchanged.
// s ^ column keeps s's block structure: the column's low bits (SwapLanes)
// permute lanes within a block and the rest pick the partner block.
//
// GCC/Clang vector types at the CPU's own width: four doubles with AVX, two
// otherwise (SSE2, NEON). Four lanes emulated on SSE2 ran slower than scalar.
#if defined(__AVX__)
constexpr std::size_t kPriceLanes = 4;
#else
constexpr std::size_t kPriceLanes = 2;
#endif
using PriceLanes = double __attribute__((vector_size(kPriceLanes * sizeof(double))));
using DecisionLanes = std::int64_t __attribute__((vector_size(kPriceLanes * sizeof(std::int64_t))));
constexpr std::size_t kBlocksPerWord = 64U / kPriceLanes;
static_assert(kStateCount % 64U == 0, "whole decision words per step");

inline void loadLanes(PriceLanes& lanes, const double* from) { std::memcpy(&lanes, from, sizeof(lanes)); }
inline void storeLanes(double* to, const PriceLanes& lanes) { std::memcpy(to, &lanes, sizeof(lanes)); }

template<unsigned SwapLanes>
void trellisStep(const double* prices, double* next, std::uint32_t column,
                 double zero_add, double one_add, std::array<std::uint64_t, 2>& decisions) {
    static_assert(kStateCount / 64U == 2U, "two decision words");
    const std::size_t partner_xor = column / kPriceLanes;
    PriceLanes zero_adds;
    PriceLanes one_adds;
    DecisionLanes lane_bits; // lane i: 1 << i
    for (std::size_t lane = 0; lane < kPriceLanes; ++lane) {
        zero_adds[lane] = zero_add;
        one_adds[lane] = one_add;
        lane_bits[lane] = std::int64_t{1} << lane;
    }
    DecisionLanes bit_lanes{};
    for (std::size_t block = 0; block < kStateCount / kPriceLanes; ++block) {
        PriceLanes here;
        PriceLanes partner;
        loadLanes(here, prices + block * kPriceLanes);
        loadLanes(partner, prices + (block ^ partner_xor) * kPriceLanes);
#if defined(__AVX__)
        partner = __builtin_shufflevector(partner, partner,
            0U ^ SwapLanes, 1U ^ SwapLanes, 2U ^ SwapLanes, 3U ^ SwapLanes);
#else
        partner = __builtin_shufflevector(partner, partner, 0U ^ SwapLanes, 1U ^ SwapLanes);
#endif
        const PriceLanes zero_price = here + zero_adds;
        const PriceLanes one_price = partner + one_adds;
        const DecisionLanes choose_one = one_price <= zero_price; // all-ones or zero
        // Vector casts reinterpret the bits; the chosen price is copied exactly.
        const DecisionLanes chosen = (choose_one & (DecisionLanes)one_price) |
            (~choose_one & (DecisionLanes)zero_price);
        storeLanes(next + block * kPriceLanes, (PriceLanes)chosen);
        // Lane i of block b is state b * kPriceLanes + i. The bits gather in
        // the lanes and are folded once per 64-state word.
        bit_lanes |= (choose_one & lane_bits) <<
            static_cast<std::int64_t>((block % kBlocksPerWord) * kPriceLanes);
        if (block % kBlocksPerWord == kBlocksPerWord - 1U) {
            std::int64_t word = 0;
            for (std::size_t lane = 0; lane < kPriceLanes; ++lane) word |= bit_lanes[lane];
            decisions[block / kBlocksPerWord] = static_cast<std::uint64_t>(word);
            bit_lanes = DecisionLanes{};
        }
    }
}

[[nodiscard]] bool getPathBit(const PathBits& path, std::size_t state) {
    return ((path.words[state / 64U] >> (state % 64U)) & 1ULL) != 0ULL;
}

[[nodiscard]] std::vector<Segment> makeSegments(std::size_t cover_count,
                                                std::size_t message_count) {
    std::vector<Segment> segments;
    std::size_t cover_offset = 0;
    std::size_t message_offset = 0;
    std::size_t segment_cover = 0;
    std::size_t segment_messages = 0;

    forEachWidth(cover_count, message_count, [&](std::size_t width) {
        if (segment_messages != 0 &&
            segment_cover + width > kMaxSegmentCover) {
            segments.push_back(Segment{
                .cover_offset = cover_offset,
                .cover_count = segment_cover,
                .message_offset = message_offset,
                .message_count = segment_messages
            });
            cover_offset += segment_cover;
            message_offset += segment_messages;
            segment_cover = 0;
            segment_messages = 0;
        }
        segment_cover += width;
        ++segment_messages;
    });
    if (segment_messages != 0) {
        segments.push_back(Segment{
            .cover_offset = cover_offset,
            .cover_count = segment_cover,
            .message_offset = message_offset,
            .message_count = segment_messages
        });
    }
    if (cover_offset + segment_cover != cover_count ||
        message_offset + segment_messages != message_count) {
        fail("segment schedule did not consume its inputs.");
    }
    return segments;
}

// Embeds one segment, writing its stego bits to `stego` (cover-sized) and
// returning how many differ from the cover.
[[nodiscard]] std::uint64_t embedRaw(
    std::span<const std::uint8_t> cover,
    std::span<const float> costs,
    std::span<const std::uint8_t> message,
    std::span<std::uint8_t> stego) {
    if (cover.size() != costs.size() || stego.size() != cover.size() || message.empty()) {
        fail("invalid raw embedding buffers.");
    }
    const std::vector<std::size_t> widths =
        makeWidths(cover.size(), message.size());
    const double infinity = std::numeric_limits<double>::infinity();
    // Two price rows, alternated by pointer swap rather than copied. The row is
    // 1 KiB and the swap happens once per cover symbol (2.5x the message bits),
    // so assigning one array to the other moved gigabytes per embed for no
    // reason. Every entry of the destination row is written before it is read
    // back, in both the trellis step and the syndrome-pruning step below, so
    // there is no stale state to carry across a swap.
    std::array<double, kStateCount> price_row_a{};
    std::array<double, kStateCount> price_row_b{};
    double* prices = price_row_a.data();
    double* next = price_row_b.data();
    std::fill_n(prices, kStateCount, infinity);
    prices[0] = 0.0;
    // The trellis choices encode the whitened message along the keyed order.
    carrier_common::WipedVector<PathBits> path(cover.size());
    // For messages shorter than h, start with only the actually reachable
    // syndrome rows. The historical reference routine assumes m >= h; this
    // small-profile adjustment makes one-byte payloads well-defined too.
    const std::size_t initial_height = std::min<std::size_t>(
        kStcConstraintHeight, message.size());
    std::uint32_t column_mask =
        (1U << static_cast<unsigned int>(initial_height)) - 1U;
    std::size_t cover_index = 0;

    for (std::size_t message_index = 0;
         message_index < message.size();
         ++message_index) {
        const std::size_t width = widths[message_index];
        for (std::size_t column_index = 0;
             column_index < width;
             ++column_index, ++cover_index) {
            const std::uint32_t column =
                columnFor(width, column_index) & column_mask;
            const double cost = std::isfinite(costs[cover_index])
                ? std::max(0.0, static_cast<double>(costs[cover_index]))
                : 1.0e13;
            const bool cover_bit = cover[cover_index] != 0;
            // This column's 128 decisions are built in registers and stored
            // once: each state's bit is decided exactly once, so there is no
            // earlier value to merge with.
            std::array<std::uint64_t, 2> decisions{};
            const double zero_add = cover_bit ? cost : 0.0;
            const double one_add = cover_bit ? 0.0 : cost;
            switch (column % kPriceLanes) {
            case 0: trellisStep<0>(prices, next, column, zero_add, one_add, decisions); break;
            case 1: trellisStep<1>(prices, next, column, zero_add, one_add, decisions); break;
#if defined(__AVX__)
            case 2: trellisStep<2>(prices, next, column, zero_add, one_add, decisions); break;
            case 3: trellisStep<3>(prices, next, column, zero_add, one_add, decisions); break;
#endif
            default: fail("column lane out of range.");
            }
            path[cover_index].words = decisions;
            std::swap(prices, next);
        }

        const std::size_t syndrome = message[message_index] != 0 ? 1U : 0U;
        for (std::size_t state = 0; state < kStateCount / 2U; ++state) {
            next[state] = prices[state * 2U + syndrome];
        }
        std::fill_n(next + kStateCount / 2U, kStateCount / 2U, infinity);
        std::swap(prices, next);
        if (message.size() - message_index <= kStcConstraintHeight) {
            column_mask >>= 1U;
        }
    }

    if (!std::isfinite(prices[0])) {
        fail("the requested syndrome has no finite solution.");
    }

    std::uint64_t changes = 0;
    std::size_t reverse_cover = cover.size();
    std::uint32_t reverse_mask = 0;
    std::size_t state = 0;
    for (std::size_t reverse_message = message.size();
         reverse_message-- > 0;) {
        const std::size_t width = widths[reverse_message];
        state = (state << 1U) |
            (message[reverse_message] != 0 ? 1U : 0U);
        if (message.size() - reverse_message <= kStcConstraintHeight) {
            reverse_mask = (reverse_mask << 1U) | 1U;
        }
        for (std::size_t reverse_column = width;
             reverse_column-- > 0;) {
            --reverse_cover;
            const bool stego_bit = getPathBit(path[reverse_cover], state);
            stego[reverse_cover] = stego_bit ? 1U : 0U;
            if (stego_bit != (cover[reverse_cover] != 0)) ++changes;
            if (stego_bit) {
                state ^= static_cast<std::size_t>(
                    columnFor(width, reverse_column) & reverse_mask);
            }
        }
    }
    if (reverse_cover != 0) {
        fail("backtracking did not consume the cover.");
    }
    return changes;
}

// Extracts one segment's message bits into `message`.
void extractRaw(
    std::span<const std::uint8_t> stego,
    std::span<std::uint8_t> message) {
    std::size_t cover_index = 0;
    std::size_t message_index = 0;
    // Bit k of `window` is the pending syndrome of message bit
    // message_index + k: a set stego bit XORs its column into the next
    // kStcConstraintHeight message bits at once. Message bit message_index
    // has all its contributions once its own columns are done; bits a column
    // would add past the last message bit are never emitted.
    std::uint32_t window = 0;
    forEachWidth(stego.size(), message.size(), [&](std::size_t width) {
        for (std::size_t column_index = 0;
             column_index < width;
             ++column_index, ++cover_index) {
            if (stego[cover_index] != 0) {
                window ^= columnFor(width, column_index);
            }
        }
        message[message_index++] = static_cast<std::uint8_t>(window & 1U);
        window >>= 1U;
    });
}

} // namespace

std::uint64_t requiredCoverSymbols(std::uint64_t message_bits) {
    if (message_bits >
        (std::numeric_limits<std::uint64_t>::max() - 1U) /
            kRateDenominator) {
        throw std::runtime_error("STC cover-size calculation overflow.");
    }
    return (message_bits * kRateDenominator +
            kRateNumerator - 1U) / kRateNumerator;
}

StcEmbedResult stcEmbed(std::span<const std::uint8_t> cover_bits,
                        std::span<const float> flip_costs,
                        std::span<const std::uint8_t> message_bits) {
    if (cover_bits.size() != flip_costs.size()) {
        fail("cover and cost lengths differ.");
    }
    if (message_bits.empty()) {
        return StcEmbedResult{
            .stego_bits = std::vector<std::uint8_t>(cover_bits.begin(),
                                                    cover_bits.end()),
            .changes = 0,
        };
    }
    if (cover_bits.size() != requiredCoverSymbols(message_bits.size())) {
        fail("cover length does not match the fixed 2/5 code rate.");
    }

    StcEmbedResult result;
    const carrier_common::WipeOnThrow stego_wipe{result.stego_bits};
    result.stego_bits.resize(cover_bits.size());
    // Segments are independent: each reads its own sub-spans and writes its
    // own range of stego_bits, so they run in parallel and the result is the
    // same for any thread count. A segment is at most kMaxSegmentCover
    // symbols, milliseconds of work, hence one per chunk.
    //
    // The helper joins workers before propagating segment errors/cancellation,
    // and completes on the caller if worker creation is refused.
    const std::vector<Segment> segments =
        makeSegments(cover_bits.size(), message_bits.size());
    const auto segment_count = static_cast<std::int64_t>(segments.size());
    std::vector<std::uint64_t> segment_changes(segments.size(), 0);
    carrier_parallel::forEach(0, segment_count, 1, [&](std::int64_t index) {
        const Segment& segment = segments[static_cast<std::size_t>(index)];
        segment_changes[static_cast<std::size_t>(index)] = embedRaw(
            cover_bits.subspan(segment.cover_offset, segment.cover_count),
            flip_costs.subspan(segment.cover_offset, segment.cover_count),
            message_bits.subspan(segment.message_offset, segment.message_count),
            std::span(result.stego_bits).subspan(segment.cover_offset, segment.cover_count));
    });
    for (const std::uint64_t changes : segment_changes) result.changes += changes;
    return result;
}

std::vector<std::uint8_t> stcExtract(
    std::span<const std::uint8_t> stego_bits,
    std::size_t message_bit_count) {
    if (message_bit_count == 0) {
        return {};
    }
    if (stego_bits.size() != requiredCoverSymbols(message_bit_count)) {
        fail("stego length does not match the fixed 2/5 code rate.");
    }
    std::vector<std::uint8_t> message(message_bit_count);
    const carrier_common::WipeOnThrow message_wipe{message};
    // Independent segments, each writing its own range of message, as in
    // stcEmbed: the result is the same for any thread count.
    const std::vector<Segment> segments = makeSegments(stego_bits.size(), message_bit_count);
    carrier_parallel::forEach(0, static_cast<std::int64_t>(segments.size()), 1, [&](std::int64_t index) {
        const Segment& segment = segments[static_cast<std::size_t>(index)];
        extractRaw(
            stego_bits.subspan(segment.cover_offset, segment.cover_count),
            std::span(message).subspan(segment.message_offset, segment.message_count));
    });
    return message;
}

} // namespace twitter_steg_internal
