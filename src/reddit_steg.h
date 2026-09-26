#pragma once

#include "carrier_common.h"
#include "common.h"

#include <cstdint>
#include <optional>
#include <span>

// Reddit currently recompresses uploaded JPEGs.  This carrier first normalizes
// the cover to a metadata-free baseline Q75/4:2:0 JPEG, then stores every
// payload bit in three independently permuted luminance DCT coefficients using
// keyed-dither QIM (format RQSTEG2). Keying hides where the bits are; it does
// not make the image statistically undetectable.
struct RedditPreparedCover {
    vBytes jpeg{};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint64_t luminance_blocks{0};
    std::size_t payload_capacity{0};
};

struct RedditEncryptedEnvelope {
    vBytes kdf_metadata{};
    vBytes encrypted_data{};
    bool is_compressed{true};
};

[[nodiscard]] RedditPreparedCover prepareRedditCover(std::span<const Byte> input_jpeg);

// Size of the compact jdvrif envelope that carries KDF metadata alongside the
// encrypted stream.  The rqsteg carrier adds its own robust length/CRC header,
// so that header is not counted here.
[[nodiscard]] std::size_t redditEnvelopeSize(std::size_t encrypted_size);

[[nodiscard]] vBytes makeRedditEnvelope(
    std::span<const Byte> kdf_metadata,
    std::span<const Byte> encrypted_data,
    bool is_compressed);

// `carrier_key` (CarrierKeys::current) fixes every coefficient position,
// whitening bit and dither offset; see deriveCarrierKeyFromPin(). Always writes
// the current RQSTEG2 format.
[[nodiscard]] vBytes embedRedditPayload(
    const RedditPreparedCover& cover,
    std::uint64_t carrier_key,
    std::span<const Byte> payload);

// Tries RQSTEG2 under `keys.current`, then the v9.0 RQSTEG1 format under
// `keys.legacy`, from a single coefficient decode. Returns nullopt when the
// JPEG is not a compatible Reddit carrier, when no carrier decodes under either
// key (a wrong PIN and an image with no carrier give the same answer), or when
// it carries rqsteg data that is not a jdvrif Reddit envelope. Once a carrier
// header parses under a key, structural/CRC failures are reported as corruption.
[[nodiscard]] std::optional<RedditEncryptedEnvelope> extractRedditEnvelope(
    std::span<const Byte> input_jpeg,
    const CarrierKeys& keys);
