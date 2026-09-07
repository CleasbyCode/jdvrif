#include "twitter_stc.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
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

[[nodiscard]] std::vector<std::size_t> makeWidths(std::size_t cover_count,
                                                   std::size_t message_count) {
    if (message_count == 0 || cover_count < message_count) {
        fail("invalid cover/message lengths.");
    }
    std::vector<std::size_t> widths(message_count);
    std::size_t consumed = 0;
    for (std::size_t message = 0; message < message_count; ++message) {
        // Round each cumulative boundary to the nearest integer. This is the
        // integer equivalent of the balanced "worm" schedule in the original
        // STC implementation.
        const std::uint64_t numerator =
            static_cast<std::uint64_t>(message + 1U) * cover_count;
        const std::size_t boundary = static_cast<std::size_t>(
            (numerator + message_count / 2U) / message_count);
        widths[message] = boundary - consumed;
        consumed = boundary;
        if (widths[message] < 2 || widths[message] > 3) {
            fail("this profile requires STC column widths of two or three.");
        }
    }
    if (consumed != cover_count) {
        fail("column schedule did not consume the cover.");
    }
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

void setPathBit(PathBits& path, std::size_t state, bool value) {
    const std::size_t word = state / 64U;
    const std::uint64_t mask = 1ULL << (state % 64U);
    if (value) {
        path.words[word] |= mask;
    } else {
        path.words[word] &= ~mask;
    }
}

[[nodiscard]] bool getPathBit(const PathBits& path, std::size_t state) {
    return ((path.words[state / 64U] >> (state % 64U)) & 1ULL) != 0ULL;
}

[[nodiscard]] std::vector<Segment> makeSegments(std::size_t cover_count,
                                                std::size_t message_count) {
    const std::vector<std::size_t> widths =
        makeWidths(cover_count, message_count);
    std::vector<Segment> segments;
    std::size_t cover_offset = 0;
    std::size_t message_offset = 0;
    std::size_t segment_cover = 0;
    std::size_t segment_messages = 0;

    for (const std::size_t width : widths) {
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
    }
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

[[nodiscard]] StcEmbedResult embedRaw(
    std::span<const std::uint8_t> cover,
    std::span<const float> costs,
    std::span<const std::uint8_t> message) {
    if (cover.size() != costs.size() || message.empty()) {
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
    std::vector<PathBits> path(cover.size());
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
            for (std::size_t state = 0; state < kStateCount; ++state) {
                const double zero_price = prices[state] +
                    (cover_bit ? cost : 0.0);
                const std::size_t alternate =
                    state ^ static_cast<std::size_t>(column);
                const double one_price = prices[alternate] +
                    (cover_bit ? 0.0 : cost);
                const bool choose_one = one_price <= zero_price;
                next[state] = choose_one ? one_price : zero_price;
                setPathBit(path[cover_index], state, choose_one);
            }
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

    StcEmbedResult result;
    result.stego_bits.resize(cover.size());
    result.distortion = prices[0];
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
            result.stego_bits[reverse_cover] = stego_bit ? 1U : 0U;
            if (stego_bit) {
                state ^= static_cast<std::size_t>(
                    columnFor(width, reverse_column) & reverse_mask);
            }
        }
    }
    if (reverse_cover != 0) {
        fail("backtracking did not consume the cover.");
    }
    for (std::size_t index = 0; index < cover.size(); ++index) {
        if (result.stego_bits[index] != (cover[index] != 0 ? 1U : 0U)) {
            ++result.changes;
        }
    }
    return result;
}

[[nodiscard]] std::vector<std::uint8_t> extractRaw(
    std::span<const std::uint8_t> stego,
    std::size_t message_count) {
    const std::vector<std::size_t> widths =
        makeWidths(stego.size(), message_count);
    std::vector<std::uint8_t> message(message_count, 0);
    std::size_t cover_index = 0;
    for (std::size_t message_index = 0;
         message_index < message_count;
         ++message_index) {
        const std::size_t width = widths[message_index];
        for (std::size_t column_index = 0;
             column_index < width;
             ++column_index, ++cover_index) {
            if (stego[cover_index] == 0) {
                continue;
            }
            const std::uint32_t column = columnFor(width, column_index);
            for (std::size_t bit = 0;
                 bit < kStcConstraintHeight &&
                 message_index + bit < message_count;
                 ++bit) {
                message[message_index + bit] ^=
                    static_cast<std::uint8_t>((column >> bit) & 1U);
            }
        }
    }
    return message;
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
            .distortion = 0.0
        };
    }
    if (cover_bits.size() != requiredCoverSymbols(message_bits.size())) {
        fail("cover length does not match the fixed 2/5 code rate.");
    }

    StcEmbedResult result;
    result.stego_bits.resize(cover_bits.size());
    for (const Segment& segment :
         makeSegments(cover_bits.size(), message_bits.size())) {
        const StcEmbedResult partial = embedRaw(
            cover_bits.subspan(segment.cover_offset, segment.cover_count),
            flip_costs.subspan(segment.cover_offset, segment.cover_count),
            message_bits.subspan(segment.message_offset, segment.message_count));
        std::copy(partial.stego_bits.begin(), partial.stego_bits.end(),
                  result.stego_bits.begin() +
                      static_cast<std::ptrdiff_t>(segment.cover_offset));
        result.changes += partial.changes;
        result.distortion += partial.distortion;
    }
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
    for (const Segment& segment :
         makeSegments(stego_bits.size(), message_bit_count)) {
        const std::vector<std::uint8_t> partial = extractRaw(
            stego_bits.subspan(segment.cover_offset, segment.cover_count),
            segment.message_count);
        std::copy(partial.begin(), partial.end(),
                  message.begin() +
                      static_cast<std::ptrdiff_t>(segment.message_offset));
    }
    return message;
}

} // namespace twitter_steg_internal
