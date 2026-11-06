#pragma once

#include "carrier_common.h"
#include "common.h"

#include <cstdint>
#include <optional>
#include <span>

// Reddit currently recompresses uploaded JPEGs.  This carrier first normalizes
// the cover to a metadata-free baseline Q75/4:2:0 JPEG, then stores every
// payload bit in three independently permuted luminance DCT coefficients using
// keyed-dither QIM (format RQSTEG3). Keying hides where the bits are; it does
// not make the image statistically undetectable.
struct RedditPreparedCover {
    vBytes jpeg{};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint64_t luminance_blocks{0};
    std::size_t payload_capacity{0};
};

using RedditEncryptedEnvelope = carrier_common::EncryptedEnvelope;

[[nodiscard]] RedditPreparedCover prepareRedditCover(std::span<const Byte> input_jpeg);

// Size of the compact jdvrif envelope that carries KDF metadata alongside the
// encrypted stream.  The rqsteg carrier adds its own robust length/CRC header,
// so that header is not counted here.
[[nodiscard]] std::size_t redditEnvelopeSize(std::size_t encrypted_size);

[[nodiscard]] vBytes makeRedditEnvelope(
    std::span<const Byte> kdf_metadata,
    std::span<const Byte> encrypted_data,
    bool is_compressed);

// The 128-bit `carrier_key` fixes every coefficient
// position, whitening bit and dither offset through SipHash-2-4. Always writes
// the current RQSTEG3 format.
[[nodiscard]] vBytes embedRedditPayload(
    const RedditPreparedCover& cover,
    const CarrierKey128& carrier_key,
    std::span<const Byte> payload);

// Decodes RQSTEG3 under `carrier_key`. Returns nullopt when the JPEG is not a
// compatible Reddit carrier, when no carrier decodes under the key (a
// wrong KEY and an image with no carrier give the same answer), or when it
// carries rqsteg data that is not a jdvrif Reddit envelope. Once a carrier
// header parses under a key, structural/CRC failures are reported as
// corruption.
[[nodiscard]] std::optional<RedditEncryptedEnvelope> extractRedditEnvelope(
    std::span<const Byte> input_jpeg,
    const CarrierKey128& carrier_key);
