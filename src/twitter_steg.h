#pragma once

#include "carrier_common.h"
#include "common.h"
#include "twitter_jpeg_codec.h"

#include <cstdint>
#include <optional>
#include <span>

// X-Twitter preserves progressive JPEG coefficients in the tested upload
// path. This carrier uses source-derived JPEG quantization, content-adaptive
// J-UNIWARD costs, and a fixed-rate syndrome-trellis code (h=7, rate 2/5)
// over luminance AC coefficients.
struct TwitterPreparedCover {
    // The JPEG the carrier is written from. For an ordinary 4:2:0 cover this
    // is the (normalized) source itself, rewritten losslessly as progressive
    // JFIF only when the payload is embedded; other covers get a re-encode.
    vBytes jpeg{};
    // Luminance DCT coefficients of `jpeg`, kept from the preparation pass so
    // embedTwitterPayload does not have to decode the same JPEG a second
    // time. embedTwitterPayload embeds into them in place.
    twitter_steg_internal::CoefficientImage coefficients{};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint64_t candidate_count{0};
    std::uint64_t nonzero_ac_count{0};
    std::size_t payload_capacity{0};
    int source_quality{0};
    int carrier_quality{0};
};

using TwitterEncryptedEnvelope = carrier_common::EncryptedEnvelope;

struct TwitterCoverDimensions {
    std::uint32_t width{0};
    std::uint32_t height{0};
};

// Header-only validation used before normalization/transcoding, including
// X-Twitter's 4096x4096 dimension ceiling.
[[nodiscard]] TwitterCoverDimensions inspectTwitterCover(
    std::span<const Byte> input_jpeg);

[[nodiscard]] TwitterPreparedCover prepareTwitterCover(
    std::span<const Byte> input_jpeg);

// Size of the compact jdvrif envelope carried by J-UNIWARD/STC. The carrier
// adds its own keyed 24-byte STC header, so that header is not counted here.
[[nodiscard]] std::size_t twitterEnvelopeSize(std::size_t encrypted_size);

[[nodiscard]] vBytes makeTwitterEnvelope(
    std::span<const Byte> kdf_metadata,
    std::span<const Byte> encrypted_data,
    bool is_compressed);

// The 128-bit `carrier_key` (Argon2id-derived from the
// recovery KEY) controls coefficient order, header/payload whitening, and
// parity-change direction through SipHash-2-4. Always writes the current
// JXSTEG4 format. Embeds into the cover's own coefficients (one fewer
// luminance-sized array), so the cover is spent; pass a copy to keep it.
[[nodiscard]] vBytes embedTwitterPayload(
    TwitterPreparedCover&& cover,
    const CarrierKey128& carrier_key,
    std::span<const Byte> payload);

// Low-level JXSTEG4 carrier decoder retained for format tests. A wrong key or non-carrier returns nullopt; authenticated
// jdvrif decryption remains a separate layer.
[[nodiscard]] std::optional<vBytes> extractTwitterPayload(
    std::span<const Byte> input_jpeg,
    const CarrierKey128& carrier_key);

// Returns nullopt for a wrong KEY/non-carrier or for non-jdvrif data carried by
// the same low-level format. Once both keyed carrier and JDVRIFX2 markers are
// present, malformed lengths/flags are reported as corruption.
[[nodiscard]] std::optional<TwitterEncryptedEnvelope> extractTwitterEnvelope(
    std::span<const Byte> input_jpeg,
    const CarrierKey128& carrier_key);
