#include "base64.h"
#include "file_utils.h"

#include <limits>
#include <stdexcept>
#include <string_view>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
#include <immintrin.h>
#endif

namespace {

constexpr std::string_view BASE64_TABLE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void binaryToBase64Scalar(std::span<const Byte> binary_data, Byte* output) {
    const std::size_t input_size = binary_data.size();
    std::size_t out = 0;

    for (std::size_t i = 0; i < input_size; i += 3) {
        const Byte
            a = binary_data[i],
            b = (i + 1 < input_size) ? binary_data[i + 1] : 0,
            c = (i + 2 < input_size) ? binary_data[i + 2] : 0;

        const std::uint32_t triple = (static_cast<std::uint32_t>(a) << 16) |
                                     (static_cast<std::uint32_t>(b) << 8) |
                                      static_cast<std::uint32_t>(c);

        output[out++] = static_cast<Byte>(BASE64_TABLE[(triple >> 18) & 0x3F]);
        output[out++] = static_cast<Byte>(BASE64_TABLE[(triple >> 12) & 0x3F]);
        output[out++] = (i + 1 < input_size)
            ? static_cast<Byte>(BASE64_TABLE[(triple >> 6) & 0x3F])
            : static_cast<Byte>('=');
        output[out++] = (i + 2 < input_size)
            ? static_cast<Byte>(BASE64_TABLE[triple & 0x3F])
            : static_cast<Byte>('=');
    }
}

#if (defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)) && \
    (defined(__GNUC__) || defined(__clang__))

[[gnu::target("avx2")]]
__m256i translateBase64AsciiAvx2(__m256i values) {
    __m256i offsets = _mm256_set1_epi8(65);
    offsets = _mm256_blendv_epi8(offsets, _mm256_set1_epi8(71),
        _mm256_cmpgt_epi8(values, _mm256_set1_epi8(25)));
    offsets = _mm256_blendv_epi8(offsets, _mm256_set1_epi8(static_cast<char>(-4)),
        _mm256_cmpgt_epi8(values, _mm256_set1_epi8(51)));
    offsets = _mm256_blendv_epi8(offsets, _mm256_set1_epi8(static_cast<char>(-19)),
        _mm256_cmpeq_epi8(values, _mm256_set1_epi8(62)));
    offsets = _mm256_blendv_epi8(offsets, _mm256_set1_epi8(static_cast<char>(-16)),
        _mm256_cmpeq_epi8(values, _mm256_set1_epi8(63)));
    return _mm256_add_epi8(values, offsets);
}

[[gnu::target("avx2")]]
void binaryToBase64Avx2(std::span<const Byte> binary_data, Byte* output) {
    static constexpr std::size_t AVX2_INPUT_STRIDE  = 24;
    static constexpr std::size_t AVX2_OUTPUT_STRIDE = 32;

    const __m256i mask6 = _mm256_set1_epi32(0x3F);
    const std::size_t avx2_end = (binary_data.size() / AVX2_INPUT_STRIDE) * AVX2_INPUT_STRIDE;
    alignas(32) std::array<std::uint32_t, 8> triples{};

    std::size_t input_offset = 0;
    std::size_t output_offset = 0;

    for (; input_offset < avx2_end; input_offset += AVX2_INPUT_STRIDE, output_offset += AVX2_OUTPUT_STRIDE) {
        const Byte* in = binary_data.data() + static_cast<std::ptrdiff_t>(input_offset);

        triples[0] = (static_cast<std::uint32_t>(in[0]) << 16) | (static_cast<std::uint32_t>(in[1]) << 8) | in[2];
        triples[1] = (static_cast<std::uint32_t>(in[3]) << 16) | (static_cast<std::uint32_t>(in[4]) << 8) | in[5];
        triples[2] = (static_cast<std::uint32_t>(in[6]) << 16) | (static_cast<std::uint32_t>(in[7]) << 8) | in[8];
        triples[3] = (static_cast<std::uint32_t>(in[9]) << 16) | (static_cast<std::uint32_t>(in[10]) << 8) | in[11];
        triples[4] = (static_cast<std::uint32_t>(in[12]) << 16) | (static_cast<std::uint32_t>(in[13]) << 8) | in[14];
        triples[5] = (static_cast<std::uint32_t>(in[15]) << 16) | (static_cast<std::uint32_t>(in[16]) << 8) | in[17];
        triples[6] = (static_cast<std::uint32_t>(in[18]) << 16) | (static_cast<std::uint32_t>(in[19]) << 8) | in[20];
        triples[7] = (static_cast<std::uint32_t>(in[21]) << 16) | (static_cast<std::uint32_t>(in[22]) << 8) | in[23];

        const __m256i packed_input = _mm256_load_si256(reinterpret_cast<const __m256i*>(triples.data()));
        const __m256i idx0 = _mm256_and_si256(_mm256_srli_epi32(packed_input, 18), mask6);
        const __m256i idx1 = _mm256_and_si256(_mm256_srli_epi32(packed_input, 12), mask6);
        const __m256i idx2 = _mm256_and_si256(_mm256_srli_epi32(packed_input, 6), mask6);
        const __m256i idx3 = _mm256_and_si256(packed_input, mask6);

        const __m256i lo01 = _mm256_unpacklo_epi32(idx0, idx1);
        const __m256i hi01 = _mm256_unpackhi_epi32(idx0, idx1);
        const __m256i lo23 = _mm256_unpacklo_epi32(idx2, idx3);
        const __m256i hi23 = _mm256_unpackhi_epi32(idx2, idx3);

        const __m256i groups01 = _mm256_packus_epi32(
            _mm256_unpacklo_epi64(lo01, lo23),
            _mm256_unpackhi_epi64(lo01, lo23)
        );
        const __m256i groups23 = _mm256_packus_epi32(
            _mm256_unpacklo_epi64(hi01, hi23),
            _mm256_unpackhi_epi64(hi01, hi23)
        );
        const __m256i six_bit_values = _mm256_packus_epi16(groups01, groups23);
        const __m256i ascii = translateBase64AsciiAvx2(six_bit_values);

        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(output + static_cast<std::ptrdiff_t>(output_offset)),
            ascii
        );
    }

    if (input_offset < binary_data.size()) {
        binaryToBase64Scalar(
            binary_data.subspan(input_offset),
            output + static_cast<std::ptrdiff_t>(output_offset)
        );
    }
}

bool cpuSupportsAvx2() {
    static const bool supported = [] {
        __builtin_cpu_init();
        return __builtin_cpu_supports("avx2");
    }();
    return supported;
}

#else

bool cpuSupportsAvx2() {
    return false;
}

#endif

} // namespace

void binaryToBase64(std::span<const Byte> binary_data, vBytes& output_vec) {
    const std::size_t input_size = binary_data.size();
    const std::size_t base_offset = output_vec.size();

    const std::size_t blocks = (input_size / 3) + ((input_size % 3) != 0 ? 1 : 0);
    if (blocks > std::numeric_limits<std::size_t>::max() / 4) {
        throw std::overflow_error("Base64 encode size overflow");
    }
    const std::size_t output_size = blocks * 4;
    const std::size_t final_size = checkedAdd(base_offset, output_size, "Base64 encode destination size overflow");

    output_vec.resize(final_size);

    if (final_size == base_offset) {
        return;
    }

    Byte* output = output_vec.data() + static_cast<std::ptrdiff_t>(base_offset);
    if (input_size >= 24 && cpuSupportsAvx2()) {
        binaryToBase64Avx2(binary_data, output);
        return;
    }

    binaryToBase64Scalar(binary_data, output);
}

void appendBase64AsBinary(std::span<const Byte> base64_data, vBytes& destination_vec) {
    const std::size_t input_size = base64_data.size();

    if (input_size == 0 || (input_size % 4) != 0) {
        throw std::invalid_argument("Base64 input size must be a multiple of 4 and non-empty");
    }

    static constexpr auto BASE64_TABLE = std::to_array<int8_t>({
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
        52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
        -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
        -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
    });

    const std::size_t decoded_upper_bound = (input_size / 4) * 3;
    const std::size_t reserve_target = checkedAdd(
        destination_vec.size(),
        decoded_upper_bound,
        "Base64 decode destination size overflow");
    destination_vec.reserve(reserve_target);

    for (std::size_t i = 0; i < input_size; i += 4) {
        const Byte
            c0 = base64_data[i],
            c1 = base64_data[i + 1],
            c2 = base64_data[i + 2],
            c3 = base64_data[i + 3];

        const bool
            p2 = (c2 == '='),
            p3 = (c3 == '=');

        if (p2 && !p3) {
            throw std::invalid_argument("Invalid Base64 padding: '==' required when third char is '='");
        }
        if ((p2 || p3) && (i + 4 < input_size)) {
            throw std::invalid_argument("Padding '=' may only appear in the final quartet");
        }

        const int
            v0 = BASE64_TABLE[c0],
            v1 = BASE64_TABLE[c1],
            v2 = p2 ? 0 : BASE64_TABLE[c2],
            v3 = p3 ? 0 : BASE64_TABLE[c3];

        if (v0 < 0 || v1 < 0 || (!p2 && v2 < 0) || (!p3 && v3 < 0)) {
            throw std::invalid_argument("Invalid Base64 character encountered");
        }

        const uint32_t triple = (static_cast<uint32_t>(v0) << 18) |
                                (static_cast<uint32_t>(v1) << 12) |
                                (static_cast<uint32_t>(v2) << 6) |
                                static_cast<uint32_t>(v3);

        destination_vec.emplace_back(static_cast<Byte>((triple >> 16) & 0xFF));

        if (!p2) destination_vec.emplace_back(static_cast<Byte>((triple >> 8) & 0xFF));
        if (!p3) destination_vec.emplace_back(static_cast<Byte>(triple & 0xFF));
    }
}
