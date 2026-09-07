#pragma once

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
    vBytes jpeg{};
    // Luminance DCT coefficients of `jpeg`, kept from the preparation pass so
    // embedTwitterPayload does not have to decode the same progressive JPEG a
    // second time. embedTwitterPayload copies before mutating.
    twitter_steg_internal::CoefficientImage coefficients{};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint64_t candidate_count{0};
    std::uint64_t nonzero_ac_count{0};
    std::size_t payload_capacity{0};
    int source_quality{0};
    int carrier_quality{0};
};

struct TwitterEncryptedEnvelope {
    vBytes kdf_metadata{};
    vBytes encrypted_data{};
    bool is_compressed{true};
};

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

// `carrier_key` derives from the recovery PIN and controls coefficient order,
// header/payload whitening, and parity-change direction.
[[nodiscard]] vBytes embedTwitterPayload(
    const TwitterPreparedCover& cover,
    std::uint64_t carrier_key,
    std::span<const Byte> payload);

// Low-level carrier decoder retained for format tests and the later recovery
// integration. A wrong key or non-carrier returns nullopt; authenticated jdvrif
// decryption remains a separate layer.
[[nodiscard]] std::optional<vBytes> extractTwitterPayload(
    std::span<const Byte> input_jpeg,
    std::uint64_t carrier_key);

// Returns nullopt for a wrong PIN/non-carrier or for non-jdvrif data carried by
// the same low-level format. Once both keyed carrier and JDVRIFX2 markers are
// present, malformed lengths/flags are reported as corruption.
[[nodiscard]] std::optional<TwitterEncryptedEnvelope> extractTwitterEnvelope(
    std::span<const Byte> input_jpeg,
    std::uint64_t carrier_key);
