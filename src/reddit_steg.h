#pragma once

#include "common.h"

#include <cstdint>
#include <optional>
#include <span>

// Reddit currently recompresses uploaded JPEGs.  This carrier first normalizes
// the cover to a metadata-free baseline Q75/4:2:0 JPEG, then stores every
// payload bit in three independently permuted luminance DCT coefficients.
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

// `carrier_key` fixes every coefficient position and whitening bit; see
// deriveCarrierKeyFromPin().
[[nodiscard]] vBytes embedRedditPayload(
    const RedditPreparedCover& cover,
    std::uint64_t carrier_key,
    std::span<const Byte> payload);

// Returns nullopt when the JPEG is not a compatible Reddit carrier, when its
// carrier does not decode under `carrier_key` (a wrong PIN is indistinguishable
// from no carrier -- that is the point of keying it), or when it carries rqsteg
// data that is not a jdvrif Reddit envelope. Once a jdvrif carrier marker is
// present under this key, structural/CRC failures are reported as corruption.
[[nodiscard]] std::optional<RedditEncryptedEnvelope> extractRedditEnvelope(
    std::span<const Byte> input_jpeg,
    std::uint64_t carrier_key);
