#include "carrier_common.h"
#include "carrier_parallel.h"
#include "twitter_jpeg_codec.h"
#include "twitter_juniward.h"
#include "twitter_stc.h"

#include <array>
#include <bit>
#include <csignal>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include <cstdio>
extern "C" {
#include <jpeglib.h>
}

using namespace twitter_steg_internal;

int main(int argc, char** argv) {
    try {
        const std::string_view mode = argc > 1 ? argv[1] : "all";
        if (mode == "threads") {
            std::cout << carrier_parallel::threadCount() << '\n';
            return 0;
        }
        std::uint64_t digest = 0xcbf29ce484222325ULL;
        const auto mix = [&](std::uint64_t value) { digest = (digest ^ value) * 0x100000001b3ULL; };
        CoefficientImage image;
        image.info = {{.width = 400, .height = 400, .y_blocks_wide = 50, .y_blocks_high = 50}};
        image.luminance.resize(50 * 50 * 64);
        image.luminance_quantization.fill(3);
        for (std::size_t i = 0; i < image.luminance.size(); ++i) {
            image.luminance[i] = static_cast<std::int16_t>(static_cast<int>(i % 19) - 9);
        }
        if (mode == "all" || mode == "luminance") {
            for (auto value : luminanceFromCoefficients(image)) mix(value);
        }
        if (mode == "all" || mode == "costs") {
            std::vector<std::uint8_t> pixels(400 * 400);
            for (std::size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<std::uint8_t>(i * 37);
            std::vector<std::uint32_t> offsets;
            for (std::uint32_t i = 1; i < image.luminance.size(); i += 13) offsets.push_back(i);
            for (float value : computeJUniwardCosts(pixels, image, offsets)) mix(std::bit_cast<std::uint32_t>(value));

            // A textured 203x157 image on its MCU-padded 26x20 block grid,
            // every AC mode of every in-image block costed. The runner pins
            // this digest: costs decide which coefficients change, so a
            // faster kernel must reproduce them bit for bit.
            CoefficientImage textured;
            textured.info = {{.width = 203, .height = 157, .y_blocks_wide = 26, .y_blocks_high = 20}};
            textured.luminance.resize(26 * 20 * 64);
            for (std::size_t i = 0; i < 64; ++i) textured.luminance_quantization[i] = static_cast<std::uint16_t>(1 + i % 13);
            std::uint64_t state = 0x9E3779B97F4A7C15ULL;
            std::vector<std::uint8_t> texture(203 * 157);
            for (std::size_t i = 0; i < texture.size(); ++i) {
                state ^= state << 13U;
                state ^= state >> 7U;
                state ^= state << 17U;
                const std::size_t row = i / 203;
                texture[i] = row < 50 ? static_cast<std::uint8_t>(row + i % 203)
                           : row < 100 ? std::uint8_t{128} : static_cast<std::uint8_t>(state);
            }
            std::vector<std::uint32_t> all_modes;
            for (std::uint32_t block = 0; block < 26 * 20; ++block) {
                if ((block / 26) * 8 >= 157 || (block % 26) * 8 >= 203) continue;
                for (std::uint32_t mode_index = 1; mode_index < 64; ++mode_index) all_modes.push_back(block * 64 + mode_index);
            }
            // Out-of-range offsets (wet) and repeated offsets, shuffled in
            // the way carrier slots are, so the visit order has to put them
            // back together.
            const std::uint32_t textured_count = 26 * 20 * 64;
            const std::size_t in_image = all_modes.size();
            for (std::uint32_t extra : {textured_count, textured_count + 63U, textured_count + 64U, 0xFFFFFFFFU}) {
                all_modes.push_back(extra);
            }
            for (std::size_t i = 0; i < in_image; i += 97) all_modes.push_back(all_modes[i]);
            for (std::size_t i = all_modes.size(); i > 1; --i) {
                state ^= state << 13U;
                state ^= state >> 7U;
                state ^= state << 17U;
                std::swap(all_modes[i - 1], all_modes[state % i]);
            }
            const auto textured_costs = computeJUniwardCosts(texture, textured, all_modes);
            std::vector<float> first_cost(textured_count, -1.0F);
            for (std::size_t slot = 0; slot < all_modes.size(); ++slot) {
                const std::uint32_t offset = all_modes[slot];
                const float cost = textured_costs[slot];
                if (offset >= textured_count) {
                    if (cost != 1.0e13F) throw std::runtime_error("out-of-range offset is not wet");
                } else if (first_cost[offset] < 0.0F) {
                    first_cost[offset] = cost;
                } else if (std::bit_cast<std::uint32_t>(first_cost[offset]) != std::bit_cast<std::uint32_t>(cost)) {
                    throw std::runtime_error("repeated offset costed differently");
                }
                mix(std::bit_cast<std::uint32_t>(cost));
            }
        }
        if (mode == "all" || mode == "stc") {
            std::vector<std::uint8_t> message(100000);
            std::vector<std::uint8_t> cover(requiredCoverSymbols(message.size()));
            std::vector<float> costs(cover.size());
            for (std::size_t i = 0; i < cover.size(); ++i) {
                cover[i] = static_cast<std::uint8_t>((i * 7 + i / 11) % 2);
                costs[i] = 0.25F + static_cast<float>(i % 17);
            }
            for (std::size_t i = 0; i < message.size(); ++i) message[i] = static_cast<std::uint8_t>((i / 3) % 2);
            const auto result = stcEmbed(cover, costs, message);
            if (stcExtract(result.stego_bits, message.size()) != message) throw std::runtime_error("STC round trip failed");
            for (auto value : result.stego_bits) mix(value);
            mix(result.changes);

            // Zero, wet, infinite and NaN costs and many exact ties, so the
            // runner's pinned digest covers the trellis's tie rule (a tie
            // takes the one branch) and its cost clamping in every build.
            std::vector<std::uint8_t> odd_message(40000);
            std::vector<std::uint8_t> odd_cover(requiredCoverSymbols(odd_message.size()));
            std::vector<float> odd_costs(odd_cover.size());
            std::uint64_t state = 0x5354435354435354ULL;
            const auto draw = [&] {
                state ^= state << 13U;
                state ^= state >> 7U;
                state ^= state << 17U;
                return state;
            };
            constexpr std::array<float, 6> specials{
                0.0F, 1.0e13F, std::numeric_limits<float>::infinity(),
                std::numeric_limits<float>::quiet_NaN(), -2.0F, 1.0F};
            for (std::size_t i = 0; i < odd_cover.size(); ++i) {
                odd_cover[i] = static_cast<std::uint8_t>(draw() & 1U);
                const std::uint64_t pick = draw();
                odd_costs[i] = pick % 4U == 0 ? specials[(pick >> 8U) % specials.size()]
                                              : static_cast<float>((pick >> 16U) % 1000U) / 64.0F;
            }
            for (auto& bit : odd_message) bit = static_cast<std::uint8_t>(draw() & 1U);
            const auto odd = stcEmbed(odd_cover, odd_costs, odd_message);
            if (stcExtract(odd.stego_bits, odd_message.size()) != odd_message) {
                throw std::runtime_error("STC round trip with special costs failed");
            }
            for (auto value : odd.stego_bits) mix(value);
            mix(odd.changes);
            // Extraction of arbitrary bits, not only of embedded ones.
            std::vector<std::uint8_t> arbitrary(requiredCoverSymbols(70001));
            for (auto& bit : arbitrary) bit = static_cast<std::uint8_t>(draw() % 3U == 0 ? 1U : 0U);
            for (auto value : stcExtract(arbitrary, 70001)) mix(value);
        }
        if (mode == "all" || mode == "crc") {
            // The slicing-by-8 CRC against the bit-at-a-time definition, for
            // every tail length and alignment, plus the standard check value.
            const auto reference = [](std::span<const std::uint8_t> data) {
                std::uint32_t crc = 0xffffffffU;
                for (const std::uint8_t byte : data) {
                    crc ^= byte;
                    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
                }
                return ~crc;
            };
            constexpr std::string_view check = "123456789";
            if (carrier_common::crc32(std::span(reinterpret_cast<const std::uint8_t*>(check.data()), check.size())) !=
                0xcbf43926U) {
                throw std::runtime_error("crc32 check value");
            }
            std::vector<std::uint8_t> bytes(70001);
            std::uint64_t state = 0x9e3779b97f4a7c15ULL;
            for (auto& byte : bytes) {
                state = state * 6364136223846793005ULL + 1442695040888963407ULL;
                byte = static_cast<std::uint8_t>(state >> 56U);
            }
            for (std::size_t offset = 0; offset < 9; ++offset) {
                for (std::size_t length = 0; length < 300; ++length) {
                    const auto view = std::span<const std::uint8_t>(bytes).subspan(offset, length);
                    if (carrier_common::crc32(view) != reference(view)) throw std::runtime_error("crc32 mismatch");
                }
            }
            const std::uint32_t whole = carrier_common::crc32(bytes);
            if (whole != reference(bytes)) throw std::runtime_error("crc32 mismatch");
            mix(whole);
        }
        if (mode == "bounds") {
            const auto refused = [](auto&& call, std::string_view expected) {
                try {
                    call();
                } catch (const std::runtime_error& error) {
                    return std::string_view(error.what()).find(expected) != std::string_view::npos;
                }
                return false;
            };
            // A block grid with no pixels behind it: refused by the carrier
            // geometry check itself, not only by the decoders in front of it.
            CoefficientImage empty;
            empty.info = {{.width = 0, .height = 8, .y_blocks_wide = 1, .y_blocks_high = 1}};
            empty.luminance.assign(64, 0);
            if (!refused([&] { (void)twitter_steg_internal::countCarrierCandidates(empty); }, "no pixels")) {
                throw std::runtime_error("carrier geometry accepted a zero-width image");
            }
            // A four-component (CMYK) JPEG: readCoefficients refuses it.
            jpeg_compress_struct encoder{};
            jpeg_error_mgr errors{};
            encoder.err = jpeg_std_error(&errors);
            jpeg_create_compress(&encoder);
            unsigned char* buffer = nullptr;
            unsigned long size = 0;
            jpeg_mem_dest(&encoder, &buffer, &size);
            encoder.image_width = 64;
            encoder.image_height = 64;
            encoder.input_components = 4;
            encoder.in_color_space = JCS_CMYK;
            jpeg_set_defaults(&encoder);
            jpeg_start_compress(&encoder, TRUE);
            std::vector<JSAMPLE> row(64 * 4, 128);
            while (encoder.next_scanline < encoder.image_height) {
                JSAMPROW pointer = row.data();
                jpeg_write_scanlines(&encoder, &pointer, 1);
            }
            jpeg_finish_compress(&encoder);
            jpeg_destroy_compress(&encoder);
            const std::vector<std::uint8_t> cmyk(buffer, buffer + size);
            std::free(buffer);
            if (!refused([&] { (void)twitter_steg_internal::readCoefficients(cmyk); }, "unsupported number of components")) {
                throw std::runtime_error("readCoefficients accepted a four-component JPEG");
            }
        }
        if (mode == "envelope") {
            // parseEnvelope's length check, compared as the bytes after the
            // header so no sum can wrap: the declared length must match exactly.
            constexpr carrier_common::EnvelopeFormat format{
                .name = "Test", .magic = {'J', 'D', 'V', 'R', 'I', 'F', 'T', '1'}, .kdf_metadata_bytes = 56};
            const std::vector<std::uint8_t> kdf(56, 0x11);
            const std::vector<std::uint8_t> data(1000, 0x22);
            const std::vector<std::uint8_t> envelope = carrier_common::makeEnvelope(format, kdf, data, true);
            const auto parsed = carrier_common::parseEnvelope(format, envelope);
            if (!parsed || parsed->kdf_metadata != kdf || parsed->encrypted_data != data || !parsed->is_compressed) {
                throw std::runtime_error("envelope round trip");
            }
            const auto refused = [&](const std::vector<std::uint8_t>& bytes) {
                try {
                    (void)carrier_common::parseEnvelope(format, bytes);
                } catch (const std::runtime_error&) {
                    return true;
                }
                return false;
            };
            const auto withLength = [&](std::uint32_t length) {
                std::vector<std::uint8_t> bytes = envelope;
                for (unsigned int byte = 0; byte < 4; ++byte) bytes[12 + byte] = static_cast<std::uint8_t>(length >> (byte * 8U));
                return bytes;
            };
            if (!refused(withLength(999)) || !refused(withLength(1001)) || !refused(withLength(0xFFFFFFFFU))) {
                throw std::runtime_error("envelope accepted a wrong declared length");
            }
            std::vector<std::uint8_t> header_only(envelope.begin(), envelope.begin() + 12 + 4 + 56);
            if (refused(withLength(1000)) || !refused(header_only) ||
                !refused(std::vector<std::uint8_t>(envelope.begin(), envelope.begin() + 20))) {
                throw std::runtime_error("envelope header-length handling");
            }
            std::vector<std::uint8_t> empty_body = header_only;
            for (unsigned int byte = 0; byte < 4; ++byte) empty_body[12 + byte] = 0;
            if (refused(empty_body)) throw std::runtime_error("envelope with an empty body refused");
            mix(parsed->encrypted_data.size());
        }
        if (mode == "helper") {
            std::array<std::atomic<int>, 1000> visits{};
            carrier_parallel::forEach(0, 1000, 4, [&](std::int64_t i) { ++visits[static_cast<std::size_t>(i)]; });
            for (const auto& count : visits) if (count != 1) throw std::runtime_error("iteration skipped or repeated");
            std::atomic<int> active{0};
            struct Guard {
                std::atomic<int>& active;
                explicit Guard(std::atomic<int>& value) : active(value) { ++active; }
                ~Guard() { --active; }
            };
            bool caught = false;
            try {
                carrier_parallel::forEach(0, 1000, 1, [&](std::int64_t) {
                    Guard guard(active);
                    throw std::runtime_error("worker failure");
                });
            } catch (const std::runtime_error& error) {
                caught = std::string_view(error.what()) == "worker failure";
            }
            if (!caught || active != 0) throw std::runtime_error("exception cleanup failed");
            installProcessSignalHandlers();
            caught = false;
            try {
                carrier_parallel::forEach(0, 1000, 1, [&](std::int64_t) {
                    Guard guard(active);
                    std::raise(SIGINT);
                });
            } catch (const SignalCancellation&) { caught = true; }
            if (!caught || active != 0) throw std::runtime_error("cancellation cleanup failed");
        }
        std::cout << std::hex << digest << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
