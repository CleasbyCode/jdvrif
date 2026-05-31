#include "base64.h"
#include "file_utils.h"

#include <stdexcept>
#include <string_view>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace {

constexpr std::string_view BASE64_TABLE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr Byte BASE64_PAD = static_cast<Byte>('=');

// ===========================================================================
//                          Scalar fallback (unchanged)
// ===========================================================================

void binaryToBase64Scalar(std::span<const Byte> binary_data, Byte* output) {
    const std::size_t input_size = binary_data.size();
    const std::size_t full_groups = input_size / 3;
    const Byte* input = binary_data.data();

    for (std::size_t i = 0; i < full_groups; ++i) {
        const Byte a = input[0];
        const Byte b = input[1];
        const Byte c = input[2];

        const std::uint32_t triple =
            (static_cast<std::uint32_t>(a) << 16) |
            (static_cast<std::uint32_t>(b) << 8) |
            static_cast<std::uint32_t>(c);

        *output++ = static_cast<Byte>(BASE64_TABLE[(triple >> 18) & 0x3F]);
        *output++ = static_cast<Byte>(BASE64_TABLE[(triple >> 12) & 0x3F]);
        *output++ = static_cast<Byte>(BASE64_TABLE[(triple >> 6) & 0x3F]);
        *output++ = static_cast<Byte>(BASE64_TABLE[triple & 0x3F]);
        input += 3;
    }

    switch (input_size - full_groups * 3) {
        case 1: {
            const std::uint32_t triple = static_cast<std::uint32_t>(input[0]) << 16;
            *output++ = static_cast<Byte>(BASE64_TABLE[(triple >> 18) & 0x3F]);
            *output++ = static_cast<Byte>(BASE64_TABLE[(triple >> 12) & 0x3F]);
            *output++ = BASE64_PAD;
            *output = BASE64_PAD;
            return;
        }
        case 2: {
            const std::uint32_t triple =
                (static_cast<std::uint32_t>(input[0]) << 16) |
                (static_cast<std::uint32_t>(input[1]) << 8);
            *output++ = static_cast<Byte>(BASE64_TABLE[(triple >> 18) & 0x3F]);
            *output++ = static_cast<Byte>(BASE64_TABLE[(triple >> 12) & 0x3F]);
            *output++ = static_cast<Byte>(BASE64_TABLE[(triple >> 6) & 0x3F]);
            *output = BASE64_PAD;
            return;
        }
        default:
            return;
    }
}

[[nodiscard]] int decodeValue(Byte c) {
    return BASE64_DECODE_TABLE[c];
}

void decodeBase64Quartet(Byte c0, Byte c1, Byte c2, Byte c3, Byte*& output) {
    const int v0 = decodeValue(c0);
    const int v1 = decodeValue(c1);
    const int v2 = decodeValue(c2);
    const int v3 = decodeValue(c3);
    if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) {
        throw std::invalid_argument("Invalid Base64 character encountered");
    }

    const uint32_t triple =
        (static_cast<uint32_t>(v0) << 18) |
        (static_cast<uint32_t>(v1) << 12) |
        (static_cast<uint32_t>(v2) << 6) |
        static_cast<uint32_t>(v3);
    *output++ = static_cast<Byte>((triple >> 16) & 0xFF);
    *output++ = static_cast<Byte>((triple >> 8) & 0xFF);
    *output++ = static_cast<Byte>(triple & 0xFF);
}

void decodeFinalBase64Quartet(Byte c0, Byte c1, Byte c2, Byte c3, Byte*& output) {
    const bool p2 = (c2 == BASE64_PAD);
    const bool p3 = (c3 == BASE64_PAD);

    if (p2 && !p3) {
        throw std::invalid_argument("Invalid Base64 padding: '==' required when third char is '='");
    }

    const int v0 = decodeValue(c0);
    const int v1 = decodeValue(c1);
    const int v2 = p2 ? 0 : decodeValue(c2);
    const int v3 = p3 ? 0 : decodeValue(c3);
    if (v0 < 0 || v1 < 0 || (!p2 && v2 < 0) || (!p3 && v3 < 0)) {
        throw std::invalid_argument("Invalid Base64 character encountered");
    }
    if ((p2 && (v1 & 0x0F) != 0) || (!p2 && p3 && (v2 & 0x03) != 0)) {
        throw std::invalid_argument("Invalid Base64 padding bits");
    }

    const uint32_t triple =
        (static_cast<uint32_t>(v0) << 18) |
        (static_cast<uint32_t>(v1) << 12) |
        (static_cast<uint32_t>(v2) << 6) |
        static_cast<uint32_t>(v3);
    *output++ = static_cast<Byte>((triple >> 16) & 0xFF);
    if (!p2) {
        *output++ = static_cast<Byte>((triple >> 8) & 0xFF);
    }
    if (!p3) {
        *output++ = static_cast<Byte>(triple & 0xFF);
    }
}

[[nodiscard]] std::size_t decodedBase64Size(std::span<const Byte> base64_data) {
    const std::size_t input_size = base64_data.size();
    if (input_size == 0 || (input_size % 4) != 0) {
        throw std::invalid_argument("Base64 input size must be a multiple of 4 and non-empty");
    }

    const bool has_one_pad = base64_data[input_size - 1] == BASE64_PAD;
    const bool has_two_pad = base64_data[input_size - 2] == BASE64_PAD;
    if (has_two_pad && !has_one_pad) {
        throw std::invalid_argument("Invalid Base64 padding: '==' required when third char is '='");
    }

    const std::size_t padding = has_two_pad ? 2 : (has_one_pad ? 1 : 0);
    return (input_size / 4) * 3 - padding;
}

// ===========================================================================
//                          AVX2 fast paths
// ===========================================================================
//
// Algorithm: Wojciech Muła's vector base64 (encode + decode).
//
// ENCODE: 24 input bytes -> 32 base64 chars per iteration.
//   1. permutevar8x32_epi32 places input bytes 0..11 in low 128b lane positions
//      0..11 and bytes 12..23 in high 128b lane positions 0..11.
//   2. pshufb spreads each 12-byte chunk into 16 bytes as [b1,b0,b2,b1] quads.
//   3. mulhi_epu16 and mullo_epi16 with magic constants extract the 4 sextets
//      per group using shift-by-multiply.
//   4. A 16-entry pshufb LUT, keyed by a category derived via subs_epu8 +
//      cmpgt_epi8, returns the ASCII offset to add to each sextet.
//
// DECODE: 32 base64 chars -> 24 bytes per iteration.
//   1. Two pshufb LUTs (one on hi-nibble, one on lo-nibble) AND-merged: nonzero
//      result anywhere signals an invalid character (errors thrown by caller).
//   2. A hi-nibble-keyed roll-LUT (with a special bump for '/') maps each
//      char to its sextet via a single add.
//   3. maddubs_epi16 + madd_epi16 with magic constants pack 4 sextets into
//      3 bytes per 32-bit lane.
//   4. pshufb + permutevar8x32 + maskstore writes exactly 24 valid bytes.
//
// References:
//   Wojciech Muła, https://github.com/WojciechMula/base64simd
//   Alfred Klomp,  https://github.com/aklomp/base64
//
#if defined(__AVX2__)

// Encode: returns count of input bytes consumed (always a multiple of 24).
[[nodiscard]] std::size_t binaryToBase64Avx2(std::span<const Byte> input, Byte* output) {
    // Each iter loads 32 bytes from in_p; the last 8 must lie within input.
    // Iter k starts at offset k*24; the load reads [k*24, k*24+32). Require
    // k*24 + 32 <= input.size() → k <= (input.size() - 32) / 24.
    if (input.size() < 32) return 0;
    const std::size_t loop_iters = (input.size() - 32) / 24 + 1;

    // Per-lane shuffle pattern. After permutevar8x32 below, each 128-bit lane
    // holds 12 input bytes at positions 0..11; this pshufb spreads them to 16
    // bytes laid out as [b1,b0,b2,b1] quads — the layout Muła's sextet
    // extraction expects.
    const __m256i shuf = _mm256_setr_epi8(
         1,  0,  2,  1,    4,  3,  5,  4,    7,  6,  8,  7,   10,  9, 11, 10,
         1,  0,  2,  1,    4,  3,  5,  4,    7,  6,  8,  7,   10,  9, 11, 10);

    // Encode LUT (16 entries, broadcast across both lanes):
    //   [0]  = +65  (uppercase 'A'-0)
    //   [1]  = +71  (lowercase 'a'-26)
    //   [2..11] = -4   (digits '0'-52)
    //   [12] = -19  ('+'-62)
    //   [13] = -16  ('/'-63)
    //   [14..15] = 0 (unused)
    const __m256i ascii_lut = _mm256_setr_epi8(
         65,  71, -4, -4, -4, -4, -4, -4, -4, -4, -4, -4, -19, -16, 0, 0,
         65,  71, -4, -4, -4, -4, -4, -4, -4, -4, -4, -4, -19, -16, 0, 0);

    const Byte* in_p  = input.data();
    Byte*       out_p = output;

    for (std::size_t i = 0; i < loop_iters; ++i) {
        // Source dwords: D0=in[0..3] D1=in[4..7] D2=in[8..11] D3=in[12..15]
        //                D4=in[16..19] D5=in[20..23] D6,D7 = junk past in[23].
        // Want: low lane = D0 D1 D2 D2, high lane = D3 D4 D5 D5
        //       so each lane has its 12 input bytes at positions 0..11.
        __m256i in = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(in_p));
        in = _mm256_permutevar8x32_epi32(in, _mm256_setr_epi32(0, 1, 2, 2, 3, 4, 5, 5));

        const __m256i spread = _mm256_shuffle_epi8(in, shuf);

        // Sextet extraction via shift-by-multiply on 16-bit halves of each
        // 32-bit quad. Each quad in `spread` is little-endian [b1,b0,b2,b1]:
        //   spread_u32 = b1 | (b0<<8) | (b2<<16) | (b1<<24)
        // Constants below are Muła's; verified by hand for "Man" -> "TWFu".
        const __m256i t0 = _mm256_and_si256 (spread, _mm256_set1_epi32(0x0FC0FC00));
        const __m256i t1 = _mm256_mulhi_epu16(t0,    _mm256_set1_epi32(0x04000040));
        const __m256i t2 = _mm256_and_si256 (spread, _mm256_set1_epi32(0x003F03F0));
        const __m256i t3 = _mm256_mullo_epi16(t2,    _mm256_set1_epi32(0x01000010));
        const __m256i sextets = _mm256_or_si256(t1, t3);

        // Convert each sextet (0..63) to its ASCII base64 char via the LUT.
        // Category mapping:
        //   sextet <= 25 (uppercase)  -> category 0      -> +65
        //   sextet 26..51 (lowercase) -> category 1      -> +71
        //   sextet 52..61 (digits)    -> category 2..11  -> -4
        //   sextet 62 ('+')           -> category 12     -> -19
        //   sextet 63 ('/')           -> category 13     -> -16
        const __m256i sat   = _mm256_subs_epu8 (sextets, _mm256_set1_epi8(51));
        const __m256i mask  = _mm256_cmpgt_epi8(sextets, _mm256_set1_epi8(25));
        const __m256i cat   = _mm256_sub_epi8  (sat, mask);  // sub of -1 == +1
        const __m256i ofs   = _mm256_shuffle_epi8(ascii_lut, cat);
        const __m256i ascii = _mm256_add_epi8  (sextets, ofs);

        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out_p), ascii);

        in_p  += 24;
        out_p += 32;
    }

    return loop_iters * 24;
}

// Decode: returns count of input chars consumed (multiple of 32) on success.
// Throws std::invalid_argument with the same message as the scalar decoder if
// any character is out of the base64 alphabet.
[[nodiscard]] std::size_t appendBase64AsBinaryAvx2(std::span<const Byte> input, Byte* output) {
    // The final quartet (last 4 chars, which may contain '=' padding) is left
    // for scalar handling. SIMD runs while at least 32 + 4 = 36 chars remain.
    if (input.size() < 36) return 0;
    const std::size_t loop_iters = (input.size() - 36) / 32 + 1;

    // Aklomp / Muła validation LUTs.
    const __m256i lut_lo = _mm256_setr_epi8(
        0x15, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
        0x11, 0x11, 0x13, 0x1A, 0x1B, 0x1B, 0x1B, 0x1A,
        0x15, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
        0x11, 0x11, 0x13, 0x1A, 0x1B, 0x1B, 0x1B, 0x1A);
    const __m256i lut_hi = _mm256_setr_epi8(
        0x10, 0x10, 0x01, 0x02, 0x04, 0x08, 0x04, 0x08,
        0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
        0x10, 0x10, 0x01, 0x02, 0x04, 0x08, 0x04, 0x08,
        0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10);
    // Roll LUT: indexed by hi-nibble (with a -1 bump for the '/' special case),
    // returns the offset to add to the input char to obtain its sextet value.
    const __m256i lut_roll = _mm256_setr_epi8(
         0, 16, 19,   4, -65, -65, -71, -71,
         0,  0,  0,   0,   0,   0,   0,   0,
         0, 16, 19,   4, -65, -65, -71, -71,
         0,  0,  0,   0,   0,   0,   0,   0);

    // Output write pattern: 12 valid bytes per 128-bit lane in positions 0..11,
    // junk at 12..15. permutevar8x32 then compacts to 24 contiguous bytes; the
    // maskstore writes exactly 24 of them, leaving bytes 24..31 untouched.
    const __m256i pack_shuf = _mm256_setr_epi8(
         2,  1,  0,    6,  5,  4,   10,  9,  8,   14, 13, 12, -1, -1, -1, -1,
         2,  1,  0,    6,  5,  4,   10,  9,  8,   14, 13, 12, -1, -1, -1, -1);

    const __m256i lane_pack = _mm256_setr_epi32(0, 1, 2, 4, 5, 6, -1, -1);
    const __m256i store_mask = _mm256_setr_epi32(-1, -1, -1, -1, -1, -1, 0, 0);

    const Byte* in_p  = input.data();
    Byte*       out_p = output;

    for (std::size_t i = 0; i < loop_iters; ++i) {
        const __m256i in = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(in_p));

        // Validity check: AND-merge of lo- and hi-nibble LUT entries. Any
        // nonzero byte means at least one input char is outside the alphabet.
        const __m256i hi_n = _mm256_and_si256(_mm256_srli_epi32(in, 4), _mm256_set1_epi8(0x0F));
        const __m256i lo_n = _mm256_and_si256(in, _mm256_set1_epi8(0x0F));
        const __m256i lo_check = _mm256_shuffle_epi8(lut_lo, lo_n);
        const __m256i hi_check = _mm256_shuffle_epi8(lut_hi, hi_n);
        const __m256i check    = _mm256_and_si256(lo_check, hi_check);
        if (!_mm256_testz_si256(check, check)) {
            throw std::invalid_argument("Invalid Base64 character encountered");
        }

        // Char -> sextet: hi-nibble keys the roll LUT. '/' (0x2F, hi=2 lo=15)
        // shares hi-nibble 2 with '+' (0x2B, hi=2 lo=11); we map '/' down to
        // roll index 1 so lut_roll[1] = 16 yields sextet 47+16=63, while
        // '+' stays at lut_roll[2] = 19 yielding 43+19=62.
        const __m256i eq_slash = _mm256_cmpeq_epi8(in, _mm256_set1_epi8(0x2F));
        const __m256i roll_idx = _mm256_add_epi8(hi_n, eq_slash);  // add of -1 == -1
        const __m256i sextets  = _mm256_add_epi8(in, _mm256_shuffle_epi8(lut_roll, roll_idx));

        // Pack 32 sextets (each in its own byte) into 24 bytes.
        //   maddubs: (s0,s1) -> (s0 << 6) + s1                 [12 valid bits per 16-bit pair]
        //   madd:    (p0,p1) -> (p0 << 12) + p1                [24 valid bits per 32-bit pair]
        const __m256i merge_ab_bc = _mm256_maddubs_epi16(sextets, _mm256_set1_epi32(0x01400140));
        const __m256i merged      = _mm256_madd_epi16  (merge_ab_bc, _mm256_set1_epi32(0x00011000));

        const __m256i bytes = _mm256_shuffle_epi8(merged, pack_shuf);
        const __m256i compact = _mm256_permutevar8x32_epi32(bytes, lane_pack);

        // 24-byte store. maskstore writes exactly the lanes whose mask MSB is
        // set, so the trailing 8 bytes are not touched and we never overrun
        // the destination buffer the caller sized to the exact decoded length.
        _mm256_maskstore_epi32(reinterpret_cast<int*>(out_p), store_mask, compact);

        in_p  += 32;
        out_p += 24;
    }

    return loop_iters * 32;
}

#endif // __AVX2__

} // namespace

void binaryToBase64(std::span<const Byte> binary_data, vBytes& output_vec) {
    const std::size_t input_size = binary_data.size();
    const std::size_t base_offset = output_vec.size();
    const std::size_t output_size = checkedMul(
        checkedAdd(input_size, 2, "Base64 encode size overflow") / 3,
        4,
        "Base64 encode size overflow");
    const std::size_t final_size = checkedAdd(base_offset, output_size, "Base64 encode destination size overflow");

    output_vec.resize(final_size);
    if (output_size == 0) return;

    Byte* output = output_vec.data() + static_cast<std::ptrdiff_t>(base_offset);

#if defined(__AVX2__)
    const std::size_t simd_in = binaryToBase64Avx2(binary_data, output);
    binary_data = binary_data.subspan(simd_in);
    output    += (simd_in / 3) * 4;
#endif

    binaryToBase64Scalar(binary_data, output);
}

void appendBase64AsBinary(std::span<const Byte> base64_data, vBytes& destination_vec) {
    const std::size_t input_size   = base64_data.size();
    const std::size_t decoded_size = decodedBase64Size(base64_data);
    const std::size_t base_offset  = destination_vec.size();
    const std::size_t final_size   = checkedAdd(
        base_offset,
        decoded_size,
        "Base64 decode destination size overflow");

    destination_vec.resize(final_size);
    Byte* output = destination_vec.data() + static_cast<std::ptrdiff_t>(base_offset);
    const Byte* const expected_output_end = destination_vec.data() + static_cast<std::ptrdiff_t>(final_size);

    try {
        std::span<const Byte> remaining = base64_data;

#if defined(__AVX2__)
        // SIMD bulk-decode (validates input as it goes). Leaves at least the
        // final quartet for scalar handling so padding rules stay in one place.
        const std::size_t simd_chars = appendBase64AsBinaryAvx2(remaining, output);
        remaining = remaining.subspan(simd_chars);
        output   += (simd_chars / 4) * 3;
#endif

        // Scalar tail: full quartets (no padding) up to but not including the
        // last 4 chars, then the final quartet via the padding-aware decoder.
        const std::size_t remaining_size = remaining.size();
        const std::size_t full_quartets  = remaining_size / 4 - 1;
        for (std::size_t i = 0; i < full_quartets; ++i) {
            const std::size_t index = i * 4;
            decodeBase64Quartet(
                remaining[index],
                remaining[index + 1],
                remaining[index + 2],
                remaining[index + 3],
                output);
        }

        const std::size_t final_index = remaining_size - 4;
        decodeFinalBase64Quartet(
            remaining[final_index],
            remaining[final_index + 1],
            remaining[final_index + 2],
            remaining[final_index + 3],
            output);

        if (output != expected_output_end) {
            throw std::runtime_error("Internal Error: Base64 decoded size mismatch.");
        }
        (void)input_size;
    } catch (...) {
        destination_vec.resize(base_offset);
        throw;
    }
}
