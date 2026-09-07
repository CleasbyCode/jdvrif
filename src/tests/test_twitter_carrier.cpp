#include "twitter_steg.h"
#include "twitter_juniward.h"
#include "twitter_stc.h"

#include <jpeglib.h>
#include <sodium.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace {

[[nodiscard]] vBytes readFile(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("failed to open test input");
    return vBytes(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

[[nodiscard]] vBytes makeSmallCover() {
    jpeg_compress_struct encoder{};
    jpeg_error_mgr errors{};
    encoder.err = jpeg_std_error(&errors);
    jpeg_create_compress(&encoder);
    unsigned char* output = nullptr;
    unsigned long output_size = 0;
    jpeg_mem_dest(&encoder, &output, &output_size);
    encoder.image_width = 400;
    encoder.image_height = 400;
    encoder.input_components = 3;
    encoder.in_color_space = JCS_RGB;
    jpeg_set_defaults(&encoder);
    jpeg_set_quality(&encoder, 90, TRUE);
    jpeg_start_compress(&encoder, TRUE);
    std::array<JSAMPLE, 400 * 3> row;
    row.fill(128);
    while (encoder.next_scanline < encoder.image_height) {
        JSAMPROW scanline = row.data();
        jpeg_write_scanlines(&encoder, &scanline, 1);
    }
    jpeg_finish_compress(&encoder);
    jpeg_destroy_compress(&encoder);
    const std::unique_ptr<unsigned char, decltype(&std::free)> guard(
        output, &std::free);
    return vBytes(output, output + output_size);
}

// Independent format helpers let this test write a valid keyed header whose
// declared length is impossible, without the embedding API rejecting it first.
[[nodiscard]] std::uint64_t mix64(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] std::uint32_t crc32(std::span<const Byte> bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const Byte byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

void checkOversizedHeader(
    const TwitterPreparedCover& cover,
    std::uint64_t carrier_key,
    std::uint32_t declared_size) {

    using namespace twitter_steg_internal;
    auto coefficients = cover.coefficients;
    const auto layout = makeCarrierLayout(
        coefficients, mix64(carrier_key ^ 0x4a58535445474c31ULL));
    std::array<Byte, 24> header{
        'J', 'X', 'S', 'T', 'E', 'G', '2', 0, 2, 7, 2, 5};
    for (unsigned int byte = 0; byte < 4; ++byte) {
        header[12 + byte] = static_cast<Byte>(declared_size >> (byte * 8U));
    }
    const auto checksum = crc32(std::span<const Byte>(header).first(20));
    for (unsigned int byte = 0; byte < 4; ++byte) {
        header[20 + byte] = static_cast<Byte>(checksum >> (byte * 8U));
    }
    vBytes header_bits(header.size() * 8U);
    for (std::size_t bit = 0; bit < header_bits.size(); ++bit) {
        const auto stream = mix64(
            carrier_key ^ 0x4a58535445474831ULL ^ (bit / 64U));
        header_bits[bit] = static_cast<Byte>(
            ((header[bit / 8U] >> (bit % 8U)) ^
             (stream >> (bit % 64U))) & 1U);
    }
    const auto offsets = std::span<const std::uint32_t>(layout.coefficient_offsets)
        .first(static_cast<std::size_t>(requiredCoverSymbols(header_bits.size())));
    const auto parity = coefficientParityBits(coefficients.luminance, offsets);
    const auto embedded = stcEmbed(
        parity, std::vector<float>(offsets.size(), 1), header_bits);
    (void)applyParityBits(coefficients.luminance, offsets, embedded.stego_bits, 0);
    const auto malformed = writeProgressiveCoefficients(
        cover.jpeg, coefficients.luminance);
    try {
        (void)extractTwitterPayload(malformed, carrier_key);
    } catch (const std::runtime_error& error) {
        if (std::string_view(error.what()).find("larger than its coefficient capacity") !=
            std::string_view::npos) {
            return;
        }
        throw;
    }
    throw std::runtime_error("oversized carrier header was accepted");
}

void checkLowAmplitudeCapacity() {
    using namespace twitter_steg_internal;
    const vBytes base = makeSmallCover();
    auto coefficients = readCoefficients(base);
    for (std::size_t offset = 0; offset < coefficients.luminance.size(); ++offset) {
        if (offset % 64U != 0) coefficients.luminance[offset] = 1;
    }
    const auto cover = prepareTwitterCover(
        writeProgressiveCoefficients(base, coefficients.luminance));
    constexpr std::uint64_t carrier_key = 42;
    const vBytes payload(cover.payload_capacity, 0x42);
    const auto embedded = embedTwitterPayload(cover, carrier_key, payload);
    const auto after = countCarrierCandidates(readCoefficients(embedded));
    if (payload.empty() || after.candidate_count != cover.candidate_count ||
        after.nonzero_ac_count >= cover.nonzero_ac_count) {
        throw std::runtime_error("low-amplitude fixture did not reduce the nonzero AC count");
    }
    const auto recovered = extractTwitterPayload(embedded, carrier_key);
    if (!recovered || *recovered != payload) {
        throw std::runtime_error("full-capacity low-amplitude carrier round trip mismatch");
    }
    // All eligible AC coefficients were nonzero before embedding, so the
    // advertised limit here is also the invariant coefficient-position limit.
    checkOversizedHeader(cover, carrier_key,
        static_cast<std::uint32_t>(cover.payload_capacity + 1U));
    checkOversizedHeader(cover, carrier_key,
        std::numeric_limits<std::uint32_t>::max());
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3 || sodium_init() < 0) return 2;
        const vBytes cover_bytes = readFile(argv[1]);
        const vBytes payload = readFile(argv[2]);
        constexpr std::uint64_t carrier_key = 0x0123456789abcdefULL;
        constexpr std::size_t kdf_metadata_size = 56;

        vBytes kdf_metadata(kdf_metadata_size);
        for (std::size_t index = 0; index < kdf_metadata.size(); ++index) {
            kdf_metadata[index] = static_cast<Byte>(index ^ 0x5aU);
        }
        const vBytes carrier_payload = makeTwitterEnvelope(
            kdf_metadata,
            payload,
            /*is_compressed=*/true);

        const TwitterPreparedCover cover = prepareTwitterCover(cover_bytes);
        if (carrier_payload.size() > cover.payload_capacity) {
            throw std::runtime_error("test payload exceeds carrier capacity");
        }
        const vBytes embedded = embedTwitterPayload(
            cover,
            carrier_key,
            carrier_payload);
        const auto recovered = extractTwitterPayload(embedded, carrier_key);
        if (!recovered || *recovered != carrier_payload) {
            throw std::runtime_error("carrier round trip mismatch");
        }
        const auto envelope = extractTwitterEnvelope(embedded, carrier_key);
        if (!envelope ||
            envelope->kdf_metadata != kdf_metadata ||
            envelope->encrypted_data != payload ||
            !envelope->is_compressed) {
            throw std::runtime_error("carrier envelope round trip mismatch");
        }
        if (extractTwitterPayload(embedded, carrier_key + 1U)) {
            throw std::runtime_error("wrong carrier key was accepted");
        }
        checkLowAmplitudeCapacity();

        std::cout << "Twitter carrier unit test passed: "
                  << carrier_payload.size() << " framed bytes, capacity "
                  << cover.payload_capacity << " bytes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
